#include <algorithm>
#include <optional>
#include <Core/Settings.h>
#include <Poco/Util/Application.h>

#ifdef OS_LINUX
#    include <unistd.h>
#endif

#include <AggregateFunctions/Combinators/AggregateFunctionArray.h>
#include <AggregateFunctions/Combinators/AggregateFunctionState.h>
#include <Columns/ColumnAggregateFunction.h>
#include <Columns/ColumnSparse.h>
#include <Columns/ColumnTuple.h>
#include <Compression/CompressedWriteBuffer.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <Disks/TemporaryFileOnDisk.h>
#include <Formats/NativeWriter.h>
#include <Functions/FunctionHelpers.h>
#include <IO/Operators.h>
#include <Interpreters/AggregationUtils.h>
#include <Interpreters/Aggregator.h>
#include <Interpreters/JIT/CompiledExpressionCache.h>
#include <Interpreters/JIT/compileFunction.h>
#include <Interpreters/TemporaryDataOnDisk.h>
#include <Parsers/ASTSelectQuery.h>
#include <base/sort.h>
#include <Common/CurrentMetrics.h>
#include <Common/CurrentThread.h>
#include <Common/JSONBuilder.h>
#include <Common/MemoryTracker.h>
#include <Common/MemoryTrackerUtils.h>
#include <Common/Stopwatch.h>
#include <Common/assert_cast.h>
#include <Common/formatReadable.h>
#include <Common/logger_useful.h>
#include <Common/scope_guard_safe.h>
#include <Common/setThreadName.h>
#include <Common/threadPoolCallbackRunner.h>
#include <Common/typeid_cast.h>

namespace ProfileEvents
{
    extern const Event ExternalAggregationWritePart;
    extern const Event ExternalAggregationCompressedBytes;
    extern const Event ExternalAggregationUncompressedBytes;
    extern const Event ExternalProcessingCompressedBytesTotal;
    extern const Event ExternalProcessingUncompressedBytesTotal;
    extern const Event AggregationHashTablesInitializedAsTwoLevel;
    extern const Event OverflowThrow;
    extern const Event OverflowBreak;
    extern const Event OverflowAny;
    extern const Event AggregationOptimizedEqualRangesOfKeys;
}

namespace CurrentMetrics
{
    extern const Metric TemporaryFilesForAggregation;
    extern const Metric AggregatorThreads;
    extern const Metric AggregatorThreadsActive;
    extern const Metric AggregatorThreadsScheduled;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_AGGREGATED_DATA_VARIANT;
    extern const int TOO_MANY_ROWS;
    extern const int EMPTY_DATA_PASSED;
    extern const int CANNOT_MERGE_DIFFERENT_AGGREGATED_DATA_VARIANTS;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
}

}

namespace
{
bool worthConvertToTwoLevel(
    size_t group_by_two_level_threshold, size_t result_size, size_t group_by_two_level_threshold_bytes, auto result_size_bytes)
{
    // params.group_by_two_level_threshold will be equal to 0 if we have only one thread to execute aggregation (refer to AggregatingStep::transformPipeline).
    return (group_by_two_level_threshold && result_size >= group_by_two_level_threshold)
        || (group_by_two_level_threshold_bytes && result_size_bytes >= static_cast<Int64>(group_by_two_level_threshold_bytes));
}

DB::AggregatedDataVariants::Type convertToTwoLevelTypeIfPossible(DB::AggregatedDataVariants::Type type)
{
    using Type = DB::AggregatedDataVariants::Type;
    switch (type)
    {
#define M(NAME) \
    case Type::NAME: \
        return Type::NAME##_two_level;
        APPLY_FOR_VARIANTS_CONVERTIBLE_TO_TWO_LEVEL(M)
#undef M
        default:
            return type;
    }
    UNREACHABLE();
}

void initDataVariantsWithSizeHint(
    DB::AggregatedDataVariants & result, DB::AggregatedDataVariants::Type method_chosen, const DB::Aggregator::Params & params)
{
    const auto & stats_collecting_params = params.stats_collecting_params;
    const auto max_threads = params.group_by_two_level_threshold != 0 ? std::max(params.max_threads, 1ul) : 1;
    if (auto hint = getSizeHint(stats_collecting_params, /*tables_cnt=*/max_threads))
    {
        if (worthConvertToTwoLevel(
                params.group_by_two_level_threshold,
                hint->sum_of_sizes,
                /*group_by_two_level_threshold_bytes*/ 0,
                /*result_size_bytes*/ 0))
            method_chosen = convertToTwoLevelTypeIfPossible(method_chosen);
        result.init(method_chosen, hint->median_size);
    }
    else
    {
        result.init(method_chosen);
    }
    ProfileEvents::increment(ProfileEvents::AggregationHashTablesInitializedAsTwoLevel, result.isTwoLevel());
}

/// Collection and use of the statistics should be enabled.
void updateStatistics(const DB::ManyAggregatedDataVariants & data_variants, const DB::StatsCollectingParams & params)
{
    if (!params.isCollectionAndUseEnabled())
        return;

    std::vector<size_t> sizes(data_variants.size());
    for (size_t i = 0; i < data_variants.size(); ++i)
        sizes[i] = data_variants[i]->size();
    const auto median_size = sizes.begin() + sizes.size() / 2; // not precisely though...
    std::nth_element(sizes.begin(), median_size, sizes.end());
    const auto sum_of_sizes = std::accumulate(sizes.begin(), sizes.end(), 0ull);
    DB::getHashTablesStatistics<DB::AggregationEntry>().update({.sum_of_sizes = sum_of_sizes, .median_size = *median_size}, params);
}

DB::ColumnNumbers calculateKeysPositions(const DB::Block & header, const DB::Aggregator::Params & params)
{
    DB::ColumnNumbers keys_positions(params.keys_size);
    for (size_t i = 0; i < params.keys_size; ++i)
        keys_positions[i] = header.getPositionByName(params.keys[i]);
    return keys_positions;
}

template <typename HashTable, typename KeyHolder>
concept HasPrefetchMemberFunc = requires
{
    {std::declval<HashTable>().prefetch(std::declval<KeyHolder>())};
};

size_t getMinBytesForPrefetch()
{
    size_t l2_size = 0;

#if defined(OS_LINUX) && defined(_SC_LEVEL2_CACHE_SIZE)
    if (auto ret = sysconf(_SC_LEVEL2_CACHE_SIZE); ret != -1)
        l2_size = ret;
#endif

    /// 256KB looks like a reasonable default L2 size. 4 is empirical constant.
    return 4 * std::max<size_t>(l2_size, 256 * 1024);
}

}

namespace DB
{

Block Aggregator::getHeader(bool final) const
{
    return params.getHeader(header, final);
}

Aggregator::Params::Params(
    const Names & keys_,
    const AggregateDescriptions & aggregates_,
    bool overflow_row_,
    size_t max_rows_to_group_by_,
    OverflowMode group_by_overflow_mode_,
    size_t group_by_two_level_threshold_,
    size_t group_by_two_level_threshold_bytes_,
    size_t max_bytes_before_external_group_by_,
    bool empty_result_for_aggregation_by_empty_set_,
    TemporaryDataOnDiskScopePtr tmp_data_scope_,
    size_t max_threads_,
    size_t min_free_disk_space_,
    bool compile_aggregate_expressions_,
    size_t min_count_to_compile_aggregate_expression_,
    size_t max_block_size_,
    bool enable_prefetch_,
    bool only_merge_, // true for projections
    bool optimize_group_by_constant_keys_,
    float min_hit_rate_to_use_consecutive_keys_optimization_,
    const StatsCollectingParams & stats_collecting_params_)
    : keys(keys_)
    , keys_size(keys.size())
    , aggregates(aggregates_)
    , aggregates_size(aggregates.size())
    , overflow_row(overflow_row_)
    , max_rows_to_group_by(max_rows_to_group_by_)
    , group_by_overflow_mode(group_by_overflow_mode_)
    , group_by_two_level_threshold(group_by_two_level_threshold_)
    , group_by_two_level_threshold_bytes(group_by_two_level_threshold_bytes_)
    , max_bytes_before_external_group_by(max_bytes_before_external_group_by_)
    , empty_result_for_aggregation_by_empty_set(empty_result_for_aggregation_by_empty_set_)
    , tmp_data_scope(std::move(tmp_data_scope_))
    , max_threads(max_threads_)
    , min_free_disk_space(min_free_disk_space_)
    , compile_aggregate_expressions(compile_aggregate_expressions_)
    , min_count_to_compile_aggregate_expression(min_count_to_compile_aggregate_expression_)
    , max_block_size(max_block_size_)
    , only_merge(only_merge_)
    , enable_prefetch(enable_prefetch_)
    , optimize_group_by_constant_keys(optimize_group_by_constant_keys_)
    , min_hit_rate_to_use_consecutive_keys_optimization(min_hit_rate_to_use_consecutive_keys_optimization_)
    , stats_collecting_params(stats_collecting_params_)
{
}

size_t Aggregator::Params::getMaxBytesBeforeExternalGroupBy(size_t max_bytes_before_external_group_by, double max_bytes_ratio_before_external_group_by)
{
    std::optional<size_t> threshold;
    if (max_bytes_before_external_group_by != 0)
        threshold = max_bytes_before_external_group_by;

    if (max_bytes_ratio_before_external_group_by != 0.)
    {
        double ratio = max_bytes_ratio_before_external_group_by;
        if (ratio < 0 || ratio >= 1.)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Setting max_bytes_ratio_before_external_group_by should be >= 0 and < 1 ({})", ratio);

        auto available_system_memory = getMostStrictAvailableSystemMemory();
        if (available_system_memory.has_value())
        {
            size_t ratio_in_bytes = static_cast<size_t>(*available_system_memory * ratio);
            if (threshold)
                threshold = std::min(threshold.value(), ratio_in_bytes);
            else
                threshold = ratio_in_bytes;

            LOG_TRACE(getLogger("Aggregator"), "Adjusting memory limit before external aggregation with {} (ratio: {}, available system memory: {})",
                formatReadableSizeWithBinarySuffix(ratio_in_bytes),
                ratio,
                formatReadableSizeWithBinarySuffix(*available_system_memory));
        }
        else
        {
            LOG_WARNING(getLogger("Aggregator"), "No system memory limits configured. Ignoring max_bytes_ratio_before_external_group_by");
        }
    }

    return threshold.value_or(0);
}

Aggregator::Params::Params(
    const Names & keys_,
    const AggregateDescriptions & aggregates_,
    bool overflow_row_,
    size_t max_threads_,
    size_t max_block_size_,
    float min_hit_rate_to_use_consecutive_keys_optimization_)
    : keys(keys_)
    , keys_size(keys.size())
    , aggregates(aggregates_)
    , aggregates_size(aggregates_.size())
    , overflow_row(overflow_row_)
    , max_threads(max_threads_)
    , max_block_size(max_block_size_)
    , only_merge(true)
    , min_hit_rate_to_use_consecutive_keys_optimization(min_hit_rate_to_use_consecutive_keys_optimization_)
{
}

Block Aggregator::Params::getHeader(
    const Block & header, bool only_merge, const Names & keys, const AggregateDescriptions & aggregates, bool final)
{
    Block res;

    if (only_merge)
    {
        NameSet needed_columns(keys.begin(), keys.end());
        for (const auto & aggregate : aggregates)
            needed_columns.emplace(aggregate.column_name);

        for (const auto & column : header)
        {
            if (needed_columns.contains(column.name))
                res.insert(column.cloneEmpty());
        }

        if (final)
        {
            for (const auto & aggregate : aggregates)
            {
                auto & elem = res.getByName(aggregate.column_name);

                elem.type = aggregate.function->getResultType();
                elem.column = elem.type->createColumn();
            }
        }
    }
    else
    {
        for (const auto & key : keys)
            res.insert(header.getByName(key).cloneEmpty());

        for (const auto & aggregate : aggregates)
        {
            size_t arguments_size = aggregate.argument_names.size();
            DataTypes argument_types(arguments_size);
            for (size_t j = 0; j < arguments_size; ++j)
                argument_types[j] = header.getByName(aggregate.argument_names[j]).type;

            DataTypePtr type;
            if (final)
                type = aggregate.function->getResultType();
            else
                type = std::make_shared<DataTypeAggregateFunction>(aggregate.function, argument_types, aggregate.parameters);

            res.insert({ type, aggregate.column_name });
        }
    }

    return materializeBlock(res);
}

ColumnRawPtrs Aggregator::Params::makeRawKeyColumns(const Block & block) const
{
    ColumnRawPtrs key_columns(keys_size);

    for (size_t i = 0; i < keys_size; ++i)
    {
#ifdef DEBUG_OR_SANITIZER_BUILD
        if (block.getPositionByName(keys[i]) != i)
        {
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "Wrong key in block [{}] at position {}, expected keys: [{}]",
                block.dumpStructure(), i, fmt::join(keys, ", "));
        }
#endif
        key_columns[i] = block.safeGetByPosition(i).column.get();
    }

    return key_columns;
}

Aggregator::AggregateColumnsConstData Aggregator::Params::makeAggregateColumnsData(const Block & block) const
{
    AggregateColumnsConstData aggregate_columns(aggregates_size);

    for (size_t i = 0; i < aggregates_size; ++i)
    {
        const auto & aggregate_column_name = aggregates[i].column_name;
        aggregate_columns[i] = &typeid_cast<const ColumnAggregateFunction &>(*block.getByName(aggregate_column_name).column).getData();
    }

    return aggregate_columns;
}

void Aggregator::Params::explain(WriteBuffer & out, size_t indent) const
{
    String prefix(indent, ' ');

    {
        /// Dump keys.
        out << prefix << "Keys:";

        bool first = true;
        for (const auto & key : keys)
        {
            if (first)
                out << " ";
            else
                out << ", ";
            first = false;
            out << key;
        }

        out << '\n';
    }

    if (!aggregates.empty())
    {
        out << prefix << "Aggregates:\n";

        for (const auto & aggregate : aggregates)
            aggregate.explain(out, indent + 4);
    }
}

void Aggregator::Params::explain(JSONBuilder::JSONMap & map) const
{
    auto keys_array = std::make_unique<JSONBuilder::JSONArray>();

    for (const auto & key : keys)
        keys_array->add(key);

    map.add("Keys", std::move(keys_array));

    if (!aggregates.empty())
    {
        auto aggregates_array = std::make_unique<JSONBuilder::JSONArray>();

        for (const auto & aggregate : aggregates)
        {
            auto aggregate_map = std::make_unique<JSONBuilder::JSONMap>();
            aggregate.explain(*aggregate_map);
            aggregates_array->add(std::move(aggregate_map));
        }

        map.add("Aggregates", std::move(aggregates_array));
    }
}

#if USE_EMBEDDED_COMPILER

static CHJIT & getJITInstance()
{
    static CHJIT jit;
    return jit;
}

class CompiledAggregateFunctionsHolder final : public CompiledExpressionCacheEntry
{
public:
    explicit CompiledAggregateFunctionsHolder(CompiledAggregateFunctions compiled_function_)
        : CompiledExpressionCacheEntry(compiled_function_.compiled_module.size)
        , compiled_aggregate_functions(compiled_function_)
    {}

    ~CompiledAggregateFunctionsHolder() override
    {
        getJITInstance().deleteCompiledModule(compiled_aggregate_functions.compiled_module);
    }

    CompiledAggregateFunctions compiled_aggregate_functions;
};

#endif

Aggregator::Aggregator(const Block & header_, const Params & params_)
    : header(header_)
    , keys_positions(calculateKeysPositions(header, params_))
    , params(params_)
    , tmp_data(params.tmp_data_scope ? params.tmp_data_scope->childScope(CurrentMetrics::TemporaryFilesForAggregation) : nullptr)
    , min_bytes_for_prefetch(getMinBytesForPrefetch())
    , thread_pool{
          CurrentMetrics::AggregatorThreads,
          CurrentMetrics::AggregatorThreadsActive,
          CurrentMetrics::AggregatorThreadsScheduled,
          params.max_threads}
{
    memory_usage_before_aggregation = getCurrentQueryMemoryUsage();

    aggregate_functions.resize(params.aggregates_size);
    for (size_t i = 0; i < params.aggregates_size; ++i)
        aggregate_functions[i] = params.aggregates[i].function.get();

    /// Initialize sizes of aggregation states and its offsets.
    offsets_of_aggregate_states.resize(params.aggregates_size);
    total_size_of_aggregate_states = 0;
    all_aggregates_has_trivial_destructor = true;

    // aggregate_states will be aligned as below:
    // |<-- state_1 -->|<-- pad_1 -->|<-- state_2 -->|<-- pad_2 -->| .....
    //
    // pad_N will be used to match alignment requirement for each next state.
    // The address of state_1 is aligned based on maximum alignment requirements in states
    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        offsets_of_aggregate_states[i] = total_size_of_aggregate_states;

        total_size_of_aggregate_states += params.aggregates[i].function->sizeOfData();

        // aggregate states are aligned based on maximum requirement
        align_aggregate_states = std::max(align_aggregate_states, params.aggregates[i].function->alignOfData());

        // If not the last aggregate_state, we need pad it so that next aggregate_state will be aligned.
        if (i + 1 < params.aggregates_size)
        {
            size_t alignment_of_next_state = params.aggregates[i + 1].function->alignOfData();
            if ((alignment_of_next_state & (alignment_of_next_state - 1)) != 0)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "`alignOfData` is not 2^N");

            /// Extend total_size to next alignment requirement
            /// Add padding by rounding up 'total_size_of_aggregate_states' to be a multiplier of alignment_of_next_state.
            total_size_of_aggregate_states = (total_size_of_aggregate_states + alignment_of_next_state - 1) / alignment_of_next_state * alignment_of_next_state;
        }

        if (!params.aggregates[i].function->hasTrivialDestructor())
            all_aggregates_has_trivial_destructor = false;
    }

    method_chosen = chooseAggregationMethod();
    HashMethodContext::Settings cache_settings;
    cache_settings.max_threads = params.max_threads;
    aggregation_state_cache = AggregatedDataVariants::createCache(method_chosen, cache_settings);

#if USE_EMBEDDED_COMPILER
    compileAggregateFunctionsIfNeeded();
#endif
}

#if USE_EMBEDDED_COMPILER

void Aggregator::compileAggregateFunctionsIfNeeded()
{
    static std::unordered_map<UInt128, UInt64, UInt128Hash> aggregate_functions_description_to_count;
    static std::mutex mutex;

    if (!params.compile_aggregate_expressions)
        return;

    std::vector<AggregateFunctionWithOffset> functions_to_compile;
    String functions_description;

    is_aggregate_function_compiled.resize(aggregate_functions.size());

    /// Add values to the aggregate functions.
    for (size_t i = 0; i < aggregate_functions.size(); ++i)
    {
        const auto * function = aggregate_functions[i];
        size_t offset_of_aggregate_function = offsets_of_aggregate_states[i];

        if (function->isCompilable())
        {
            AggregateFunctionWithOffset function_to_compile
            {
                .function = function,
                .aggregate_data_offset = offset_of_aggregate_function
            };

            functions_to_compile.emplace_back(std::move(function_to_compile));

            functions_description += function->getDescription();
            functions_description += ' ';

            functions_description += std::to_string(offset_of_aggregate_function);
            functions_description += ' ';
        }

        is_aggregate_function_compiled[i] = function->isCompilable();
    }

    if (functions_to_compile.empty())
        return;

    SipHash aggregate_functions_description_hash;
    aggregate_functions_description_hash.update(functions_description);

    const auto aggregate_functions_description_hash_key = aggregate_functions_description_hash.get128();

    {
        std::lock_guard<std::mutex> lock(mutex);

        if (aggregate_functions_description_to_count[aggregate_functions_description_hash_key]++ < params.min_count_to_compile_aggregate_expression)
            return;
    }

    if (auto * compilation_cache = CompiledExpressionCacheFactory::instance().tryGetCache())
    {
        auto [compiled_function_cache_entry, _] = compilation_cache->getOrSet(aggregate_functions_description_hash_key, [&] ()
        {
            LOG_TRACE(log, "Compile expression {}", functions_description);

            auto compiled_aggregate_functions = compileAggregateFunctions(getJITInstance(), functions_to_compile, functions_description);
            return std::make_shared<CompiledAggregateFunctionsHolder>(std::move(compiled_aggregate_functions));
        });
        compiled_aggregate_functions_holder = std::static_pointer_cast<CompiledAggregateFunctionsHolder>(compiled_function_cache_entry);
    }
    else
    {
        LOG_TRACE(log, "Compile expression {}", functions_description);
        auto compiled_aggregate_functions = compileAggregateFunctions(getJITInstance(), functions_to_compile, functions_description);
        compiled_aggregate_functions_holder = std::make_shared<CompiledAggregateFunctionsHolder>(std::move(compiled_aggregate_functions));
    }
}

#endif

AggregatedDataVariants::Type Aggregator::chooseAggregationMethod()
{
    /// If no keys. All aggregating to single row.
    if (params.keys_size == 0)
        return AggregatedDataVariants::Type::without_key;

    /// Check if at least one of the specified keys is nullable.
    DataTypes types_removed_nullable;
    types_removed_nullable.reserve(params.keys.size());
    bool has_nullable_key = false;
    bool has_low_cardinality = false;

    for (const auto & key : params.keys)
    {
        DataTypePtr type = header.getByName(key).type;

        if (type->lowCardinality())
        {
            has_low_cardinality = true;
            type = removeLowCardinality(type);
        }

        if (type->isNullable())
        {
            has_nullable_key = true;
            type = removeNullable(type);
        }

        types_removed_nullable.push_back(type);
    }

    /** Returns ordinary (not two-level) methods, because we start from them.
      * Later, during aggregation process, data may be converted (partitioned) to two-level structure, if cardinality is high.
      */

    size_t keys_bytes = 0;
    size_t num_fixed_contiguous_keys = 0;

    key_sizes.resize(params.keys_size);
    for (size_t j = 0; j < params.keys_size; ++j)
    {
        if (types_removed_nullable[j]->isValueUnambiguouslyRepresentedInContiguousMemoryRegion())
        {
            if (types_removed_nullable[j]->isValueUnambiguouslyRepresentedInFixedSizeContiguousMemoryRegion())
            {
                ++num_fixed_contiguous_keys;
                key_sizes[j] = types_removed_nullable[j]->getSizeOfValueInMemory();
                keys_bytes += key_sizes[j];
            }
        }
    }

    bool all_keys_are_numbers_or_strings = true;
    for (size_t j = 0; j < params.keys_size; ++j)
    {
        if (!types_removed_nullable[j]->isValueRepresentedByNumber() && !isString(types_removed_nullable[j])
            && !isFixedString(types_removed_nullable[j]))
        {
            all_keys_are_numbers_or_strings = false;
            break;
        }
    }

    if (has_nullable_key)
    {
        /// Optimization for one key
        if (params.keys_size == 1 && !has_low_cardinality)
        {
            if (types_removed_nullable[0]->isValueRepresentedByNumber())
            {
                size_t size_of_field = types_removed_nullable[0]->getSizeOfValueInMemory();
                if (size_of_field == 1)
                    return AggregatedDataVariants::Type::nullable_key8;
                if (size_of_field == 2)
                    return AggregatedDataVariants::Type::nullable_key16;
                if (size_of_field == 4)
                    return AggregatedDataVariants::Type::nullable_key32;
                if (size_of_field == 8)
                    return AggregatedDataVariants::Type::nullable_key64;
            }
            if (isFixedString(types_removed_nullable[0]))
            {
                return AggregatedDataVariants::Type::nullable_key_fixed_string;
            }
            if (isString(types_removed_nullable[0]))
            {
                return AggregatedDataVariants::Type::nullable_key_string;
            }
        }

        if (params.keys_size == num_fixed_contiguous_keys && !has_low_cardinality)
        {
            /// Pack if possible all the keys along with information about which key values are nulls
            /// into a fixed 16- or 32-byte blob.
            if (std::tuple_size<KeysNullMap<UInt128>>::value + keys_bytes <= 16)
                return AggregatedDataVariants::Type::nullable_keys128;
            if (std::tuple_size<KeysNullMap<UInt256>>::value + keys_bytes <= 32)
                return AggregatedDataVariants::Type::nullable_keys256;
        }

        if (has_low_cardinality && params.keys_size == 1)
        {
            if (types_removed_nullable[0]->isValueRepresentedByNumber())
            {
                size_t size_of_field = types_removed_nullable[0]->getSizeOfValueInMemory();

                if (size_of_field == 1)
                    return AggregatedDataVariants::Type::low_cardinality_key8;
                if (size_of_field == 2)
                    return AggregatedDataVariants::Type::low_cardinality_key16;
                if (size_of_field == 4)
                    return AggregatedDataVariants::Type::low_cardinality_key32;
                if (size_of_field == 8)
                    return AggregatedDataVariants::Type::low_cardinality_key64;
            }
            else if (isString(types_removed_nullable[0]))
                return AggregatedDataVariants::Type::low_cardinality_key_string;
            else if (isFixedString(types_removed_nullable[0]))
                return AggregatedDataVariants::Type::low_cardinality_key_fixed_string;
        }

        if (params.keys_size > 1 && all_keys_are_numbers_or_strings)
            return AggregatedDataVariants::Type::nullable_prealloc_serialized;

        /// Fallback case.
        return AggregatedDataVariants::Type::nullable_serialized;
    }

    /// No key has been found to be nullable.

    /// Single numeric key.
    if (params.keys_size == 1 && types_removed_nullable[0]->isValueRepresentedByNumber())
    {
        size_t size_of_field = types_removed_nullable[0]->getSizeOfValueInMemory();

        if (has_low_cardinality)
        {
            if (size_of_field == 1)
                return AggregatedDataVariants::Type::low_cardinality_key8;
            if (size_of_field == 2)
                return AggregatedDataVariants::Type::low_cardinality_key16;
            if (size_of_field == 4)
                return AggregatedDataVariants::Type::low_cardinality_key32;
            if (size_of_field == 8)
                return AggregatedDataVariants::Type::low_cardinality_key64;
            if (size_of_field == 16)
                return AggregatedDataVariants::Type::low_cardinality_keys128;
            if (size_of_field == 32)
                return AggregatedDataVariants::Type::low_cardinality_keys256;
            throw Exception(ErrorCodes::LOGICAL_ERROR, "LowCardinality numeric column has sizeOfField not in 1, 2, 4, 8, 16, 32.");
        }

        if (size_of_field == 1)
            return AggregatedDataVariants::Type::key8;
        if (size_of_field == 2)
            return AggregatedDataVariants::Type::key16;
        if (size_of_field == 4)
            return AggregatedDataVariants::Type::key32;
        if (size_of_field == 8)
            return AggregatedDataVariants::Type::key64;
        if (size_of_field == 16)
            return AggregatedDataVariants::Type::keys128;
        if (size_of_field == 32)
            return AggregatedDataVariants::Type::keys256;
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Numeric column has sizeOfField not in 1, 2, 4, 8, 16, 32.");
    }

    if (params.keys_size == 1 && isFixedString(types_removed_nullable[0]))
    {
        if (has_low_cardinality)
            return AggregatedDataVariants::Type::low_cardinality_key_fixed_string;
        return AggregatedDataVariants::Type::key_fixed_string;
    }

    /// If all keys fits in N bits, will use hash table with all keys packed (placed contiguously) to single N-bit key.
    if (params.keys_size == num_fixed_contiguous_keys)
    {
        if (has_low_cardinality)
        {
            if (keys_bytes <= 16)
                return AggregatedDataVariants::Type::low_cardinality_keys128;
            if (keys_bytes <= 32)
                return AggregatedDataVariants::Type::low_cardinality_keys256;
        }

        if (keys_bytes <= 2)
            return AggregatedDataVariants::Type::keys16;
        if (keys_bytes <= 4)
            return AggregatedDataVariants::Type::keys32;
        if (keys_bytes <= 8)
            return AggregatedDataVariants::Type::keys64;
        if (keys_bytes <= 16)
            return AggregatedDataVariants::Type::keys128;
        if (keys_bytes <= 32)
            return AggregatedDataVariants::Type::keys256;
    }

    /// If single string key - will use hash table with references to it. Strings itself are stored separately in Arena.
    if (params.keys_size == 1 && isString(types_removed_nullable[0]))
    {
        if (has_low_cardinality)
            return AggregatedDataVariants::Type::low_cardinality_key_string;
        return AggregatedDataVariants::Type::key_string;
    }

    if (params.keys_size > 1 && all_keys_are_numbers_or_strings)
        return AggregatedDataVariants::Type::prealloc_serialized;

    return AggregatedDataVariants::Type::serialized;
}

template <bool skip_compiled_aggregate_functions>
void Aggregator::createAggregateStates(AggregateDataPtr & aggregate_data) const
{
    for (size_t j = 0; j < params.aggregates_size; ++j)
    {
        if constexpr (skip_compiled_aggregate_functions)
            if (is_aggregate_function_compiled[j])
                continue;

        try
        {
            /** An exception may occur if there is a shortage of memory.
              * In order that then everything is properly destroyed, we "roll back" some of the created states.
              * The code is not very convenient.
              */
            aggregate_functions[j]->create(aggregate_data + offsets_of_aggregate_states[j]);
        }
        catch (...)
        {
            for (size_t rollback_j = 0; rollback_j < j; ++rollback_j)
            {
                if constexpr (skip_compiled_aggregate_functions)
                    if (is_aggregate_function_compiled[j])
                        continue;

                aggregate_functions[rollback_j]->destroy(aggregate_data + offsets_of_aggregate_states[rollback_j]);
            }

            throw;
        }
    }
}

bool Aggregator::hasSparseArguments(AggregateFunctionInstruction * aggregate_instructions)
{
    for (auto * inst = aggregate_instructions; inst->that; ++inst)
        if (inst->has_sparse_arguments)
            return true;
    return false;
}

void Aggregator::executeOnBlockSmall(
    AggregatedDataVariants & result,
    size_t row_begin,
    size_t row_end,
    ColumnRawPtrs & key_columns,
    AggregateFunctionInstruction * aggregate_instructions) const
{
    /// `result` will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// How to perform the aggregation?
    if (result.empty())
    {
        if (method_chosen != AggregatedDataVariants::Type::without_key)
            initDataVariantsWithSizeHint(result, method_chosen, params);
        else
            result.init(method_chosen);

        result.keys_size = params.keys_size;
        result.key_sizes = key_sizes;
    }

    executeImpl(result, row_begin, row_end, key_columns, aggregate_instructions);
    CurrentMemoryTracker::check();
}

void Aggregator::mergeOnBlockSmall(
    AggregatedDataVariants & result,
    size_t row_begin,
    size_t row_end,
    const AggregateColumnsConstData & aggregate_columns_data,
    const ColumnRawPtrs & key_columns) const
{
    /// `result` will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// How to perform the aggregation?
    if (result.empty())
    {
        initDataVariantsWithSizeHint(result, method_chosen, params);
        result.keys_size = params.keys_size;
        result.key_sizes = key_sizes;
    }

    if ((params.overflow_row || result.type == AggregatedDataVariants::Type::without_key) && !result.without_key)
    {
        AggregateDataPtr place = result.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        result.without_key = place;
    }

    std::atomic<bool> is_cancelled{false};

    if (false) {} // NOLINT
#define M(NAME, IS_TWO_LEVEL) \
    else if (result.type == AggregatedDataVariants::Type::NAME) \
        mergeStreamsImpl(result.aggregates_pool, *result.NAME, result.NAME->data, \
                         result.without_key, \
                         result.consecutive_keys_cache_stats, \
                         /* no_more_keys= */ false, \
                         row_begin, row_end, \
                         aggregate_columns_data, key_columns, is_cancelled, result.aggregates_pool);

    APPLY_FOR_AGGREGATED_VARIANTS(M)
#undef M
    else
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");

    CurrentMemoryTracker::check();
}

void Aggregator::executeImpl(
    AggregatedDataVariants & result,
    size_t row_begin,
    size_t row_end,
    ColumnRawPtrs & key_columns,
    AggregateFunctionInstruction * aggregate_instructions,
    bool no_more_keys,
    bool all_keys_are_const,
    AggregateDataPtr overflow_row) const
{
    #define M(NAME, IS_TWO_LEVEL) \
        else if (result.type == AggregatedDataVariants::Type::NAME) \
            executeImpl(*result.NAME, result.aggregates_pool, row_begin, row_end, key_columns, aggregate_instructions, \
                        result.consecutive_keys_cache_stats, no_more_keys, all_keys_are_const, overflow_row);

    if (false) {} // NOLINT
    APPLY_FOR_AGGREGATED_VARIANTS(M)
    #undef M
}

template <typename Method>
void NO_INLINE Aggregator::executeImpl(
    Method & method,
    Arena * aggregates_pool,
    size_t row_begin,
    size_t row_end,
    ColumnRawPtrs & key_columns,
    AggregateFunctionInstruction * aggregate_instructions,
    LastElementCacheStats & consecutive_keys_cache_stats,
    bool no_more_keys,
    bool all_keys_are_const,
    AggregateDataPtr overflow_row) const
{
    UInt64 total_rows = consecutive_keys_cache_stats.hits + consecutive_keys_cache_stats.misses;
    double cache_hit_rate = total_rows ? static_cast<double>(consecutive_keys_cache_stats.hits) / total_rows : 1.0;
    bool use_cache = cache_hit_rate >= params.min_hit_rate_to_use_consecutive_keys_optimization;

    if (use_cache)
    {
        typename Method::State state(key_columns, key_sizes, aggregation_state_cache);
        executeImpl(method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, no_more_keys, all_keys_are_const, overflow_row);
        consecutive_keys_cache_stats.update(row_end - row_begin, state.getCacheMissesSinceLastReset());
    }
    else
    {
        typename Method::StateNoCache state(key_columns, key_sizes, aggregation_state_cache);
        executeImpl(method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, no_more_keys, all_keys_are_const, overflow_row);
    }
}

/** It's interesting - if you remove `noinline`, then gcc for some reason will inline this function, and the performance decreases (~ 10%).
  * (Probably because after the inline of this function, more internal functions no longer be inlined.)
  * Inline does not make sense, since the inner loop is entirely inside this function.
  */
template <typename Method, typename State>
void NO_INLINE Aggregator::executeImpl(
    Method & method,
    State & state,
    Arena * aggregates_pool,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    bool no_more_keys,
    bool all_keys_are_const,
    AggregateDataPtr overflow_row) const
{
    if (!no_more_keys)
    {
        /// Prefetching doesn't make sense for small hash tables, because they fit in caches entirely.
        const bool prefetch = Method::State::has_cheap_key_calculation && params.enable_prefetch
            && (method.data.getBufferSizeInBytes() > min_bytes_for_prefetch);

#if USE_EMBEDDED_COMPILER
        if (compiled_aggregate_functions_holder && !hasSparseArguments(aggregate_instructions))
        {
            if (prefetch)
                executeImplBatch<true>(
                    method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, false, all_keys_are_const, true, overflow_row);
            else
                executeImplBatch<false>(
                    method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, false, all_keys_are_const, true, overflow_row);
        }
        else
#endif
        {
            if (prefetch)
                executeImplBatch<true>(
                    method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, false, all_keys_are_const, false, overflow_row);
            else
                executeImplBatch<false>(
                    method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, false, all_keys_are_const, false, overflow_row);
        }
    }
    else
    {
        executeImplBatch<false>(method, state, aggregates_pool, row_begin, row_end, aggregate_instructions, true, all_keys_are_const, false, overflow_row);
    }
}

template <bool prefetch, typename Method, typename State>
void NO_INLINE Aggregator::executeImplBatch(
    Method & method,
    State & state,
    Arena * aggregates_pool,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    bool no_more_keys,
    bool all_keys_are_const,
    bool use_compiled_functions [[maybe_unused]],
    AggregateDataPtr overflow_row) const
{
    using KeyHolder = decltype(state.getKeyHolder(0, std::declval<Arena &>()));

    /// During processing of row #i we will prefetch HashTable cell for row #(i + prefetch_look_ahead).
    PrefetchingHelper prefetching;
    size_t prefetch_look_ahead = PrefetchingHelper::getInitialLookAheadValue();

    /// Optimization for special case when there are no aggregate functions.
    if (params.aggregates_size == 0)
    {
        if (no_more_keys)
            return;

        /// This pointer is unused, but the logic will compare it for nullptr to check if the cell is set.
        AggregateDataPtr place = reinterpret_cast<AggregateDataPtr>(0x1);
        if (all_keys_are_const)
        {
            state.emplaceKey(method.data, 0, *aggregates_pool).setMapped(place);
        }
        else
        {
            /// For all rows.
            for (size_t i = row_begin; i < row_end; ++i)
            {
                if constexpr (prefetch && HasPrefetchMemberFunc<decltype(method.data), KeyHolder>)
                {
                    if (i == row_begin + PrefetchingHelper::iterationsToMeasure())
                        prefetch_look_ahead = prefetching.calcPrefetchLookAhead();

                    if (i + prefetch_look_ahead < row_end)
                    {
                        auto && key_holder = state.getKeyHolder(i + prefetch_look_ahead, *aggregates_pool);
                        method.data.prefetch(std::move(key_holder));
                    }
                }

                state.emplaceKey(method.data, i, *aggregates_pool).setMapped(place);
            }
        }
        return;
    }

    /// Optimization for special case when aggregating by 8bit key.
    if (!no_more_keys)
    {
        if constexpr (std::is_same_v<Method, typename decltype(AggregatedDataVariants::key8)::element_type>)
        {
            /// We use another method if there are aggregate functions with -Array combinator.
            bool has_arrays = false;
            for (AggregateFunctionInstruction * inst = aggregate_instructions; inst->that; ++inst)
            {
                if (inst->offsets)
                {
                    has_arrays = true;
                    break;
                }
            }

            if (!has_arrays && !hasSparseArguments(aggregate_instructions) && !all_keys_are_const)
            {
                for (AggregateFunctionInstruction * inst = aggregate_instructions; inst->that; ++inst)
                {
                    inst->batch_that->addBatchLookupTable8(
                        row_begin,
                        row_end,
                        reinterpret_cast<AggregateDataPtr *>(method.data.data()),
                        inst->state_offset,
                        [&](AggregateDataPtr & aggregate_data)
                        {
                            AggregateDataPtr place = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
                            createAggregateStates(place);
                            aggregate_data = place;
                        },
                        state.getKeyData(),
                        inst->batch_arguments,
                        aggregates_pool);
                }
                return;
            }
        }
    }

    /// NOTE: only row_end-row_start is required, but:
    /// - this affects only optimize_aggregation_in_order,
    /// - this is just a pointer, so it should not be significant,
    /// - and plus this will require other changes in the interface.
    std::unique_ptr<AggregateDataPtr[]> places(new AggregateDataPtr[all_keys_are_const ? 1 : row_end]);

    size_t key_start;
    size_t key_end;
    /// If all keys are const, key columns contain only 1 row.
    if  (all_keys_are_const)
    {
        key_start = 0;
        key_end = 1;
    }
    else
    {
        key_start = row_begin;
        key_end = row_end;
    }

    state.resetCache();

    /// For all rows.
    if (!no_more_keys)
    {
        for (size_t i = key_start; i < key_end; ++i)
        {
            AggregateDataPtr aggregate_data = nullptr;

            if constexpr (prefetch && HasPrefetchMemberFunc<decltype(method.data), KeyHolder>)
            {
                if (i == key_start + PrefetchingHelper::iterationsToMeasure())
                    prefetch_look_ahead = prefetching.calcPrefetchLookAhead();

                if (i + prefetch_look_ahead < row_end)
                {
                    auto && key_holder = state.getKeyHolder(i + prefetch_look_ahead, *aggregates_pool);
                    method.data.prefetch(std::move(key_holder));
                }
            }

            auto emplace_result = state.emplaceKey(method.data, i, *aggregates_pool);

            /// If a new key is inserted, initialize the states of the aggregate functions, and possibly something related to the key.
            if (emplace_result.isInserted())
            {
                /// exception-safety - if you can not allocate memory or create states, then destructors will not be called.
                emplace_result.setMapped(nullptr);

                aggregate_data = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);

#if USE_EMBEDDED_COMPILER
                if (use_compiled_functions)
                {
                    const auto & compiled_aggregate_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;
                    compiled_aggregate_functions.create_aggregate_states_function(aggregate_data);
                    if (compiled_aggregate_functions.functions_count != aggregate_functions.size())
                    {
                        static constexpr bool skip_compiled_aggregate_functions = true;
                        createAggregateStates<skip_compiled_aggregate_functions>(aggregate_data);
                    }
                }
                else
#endif
                {
                    createAggregateStates(aggregate_data);
                }

                emplace_result.setMapped(aggregate_data);
            }
            else
                aggregate_data = emplace_result.getMapped();

            assert(aggregate_data != nullptr);
            places[i] = aggregate_data;
        }
    }
    else
    {
        for (size_t i = key_start; i < key_end; ++i)
        {
            AggregateDataPtr aggregate_data = nullptr;
            /// Add only if the key already exists.
            auto find_result = state.findKey(method.data, i, *aggregates_pool);
            if (find_result.isFound())
                aggregate_data = find_result.getMapped();
            else
                aggregate_data = overflow_row;
            places[i] = aggregate_data;
        }
    }

    executeAggregateInstructions(
        aggregates_pool,
        row_begin,
        row_end,
        aggregate_instructions,
        places,
        key_start,
        state.hasOnlyOneValueSinceLastReset(),
        all_keys_are_const,
        use_compiled_functions);
}

void Aggregator::executeAggregateInstructions(
    Arena * aggregates_pool,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    const std::unique_ptr<AggregateDataPtr[]> &places,
    size_t key_start,
    bool has_only_one_value_since_last_reset,
    bool all_keys_are_const,
    bool use_compiled_functions [[maybe_unused]]) const
{
#if USE_EMBEDDED_COMPILER
    if (use_compiled_functions)
    {
        std::vector<ColumnData> columns_data;
        bool can_optimize_equal_keys_ranges = true;

        for (size_t i = 0; i < aggregate_functions.size(); ++i)
        {
            if (!is_aggregate_function_compiled[i])
                continue;

            AggregateFunctionInstruction * inst = aggregate_instructions + i;
            size_t arguments_size = inst->that->getArgumentTypes().size(); // NOLINT
            can_optimize_equal_keys_ranges &= inst->can_optimize_equal_keys_ranges;

            for (size_t argument_index = 0; argument_index < arguments_size; ++argument_index)
                columns_data.emplace_back(getColumnData(inst->batch_arguments[argument_index]));
        }

        if (all_keys_are_const || (can_optimize_equal_keys_ranges && has_only_one_value_since_last_reset))
        {
            ProfileEvents::increment(ProfileEvents::AggregationOptimizedEqualRangesOfKeys);
            auto add_into_aggregate_states_function_single_place = compiled_aggregate_functions_holder->compiled_aggregate_functions.add_into_aggregate_states_function_single_place;
            add_into_aggregate_states_function_single_place(row_begin, row_end, columns_data.data(), places[key_start]);
        }
        else
        {
            auto add_into_aggregate_states_function = compiled_aggregate_functions_holder->compiled_aggregate_functions.add_into_aggregate_states_function;
            add_into_aggregate_states_function(row_begin, row_end, columns_data.data(), places.get());
        }
    }
#endif

    /// Add values to the aggregate functions.
    for (size_t i = 0; i < aggregate_functions.size(); ++i)
    {
#if USE_EMBEDDED_COMPILER
        if (use_compiled_functions && is_aggregate_function_compiled[i])
            continue;
#endif

        AggregateFunctionInstruction * inst = aggregate_instructions + i;

        if (all_keys_are_const || (inst->can_optimize_equal_keys_ranges && has_only_one_value_since_last_reset))
        {
            ProfileEvents::increment(ProfileEvents::AggregationOptimizedEqualRangesOfKeys);
            addBatchSinglePlace(row_begin, row_end, inst, places[key_start] + inst->state_offset, aggregates_pool);
        }
        else
        {
            addBatch(row_begin, row_end, inst, places.get(), aggregates_pool);
        }
    }

}


void NO_INLINE Aggregator::executeWithoutKeyImpl(
    AggregatedDataWithoutKey & res,
    size_t row_begin,
    size_t row_end,
    AggregateFunctionInstruction * aggregate_instructions,
    Arena * arena,
    bool use_compiled_functions [[maybe_unused]]) const
{
    if (row_begin == row_end)
        return;

#if USE_EMBEDDED_COMPILER
    if (use_compiled_functions)
    {
        std::vector<ColumnData> columns_data;

        for (size_t i = 0; i < aggregate_functions.size(); ++i)
        {
            if (!is_aggregate_function_compiled[i])
                continue;

            AggregateFunctionInstruction * inst = aggregate_instructions + i;
            size_t arguments_size = inst->that->getArgumentTypes().size();

            for (size_t argument_index = 0; argument_index < arguments_size; ++argument_index)
            {
                columns_data.emplace_back(getColumnData(inst->batch_arguments[argument_index]));
            }
        }

        auto add_into_aggregate_states_function_single_place = compiled_aggregate_functions_holder->compiled_aggregate_functions.add_into_aggregate_states_function_single_place;
        add_into_aggregate_states_function_single_place(row_begin, row_end, columns_data.data(), res);
    }
#endif

    /// Adding values
    for (size_t i = 0; i < aggregate_functions.size(); ++i)
    {
        AggregateFunctionInstruction * inst = aggregate_instructions + i;
#if USE_EMBEDDED_COMPILER
        if (use_compiled_functions && is_aggregate_function_compiled[i])
            continue;
#endif
        addBatchSinglePlace(row_begin, row_end, inst, res + inst->state_offset, arena);
    }
}

void Aggregator::addBatch(
    size_t row_begin, size_t row_end,
    AggregateFunctionInstruction * inst,
    AggregateDataPtr * places,
    Arena * arena)
{
    if (inst->offsets)
        inst->batch_that->addBatchArray(
            row_begin, row_end, places,
            inst->state_offset,
            inst->batch_arguments,
            inst->offsets,
            arena);
    else if (inst->has_sparse_arguments)
        inst->batch_that->addBatchSparse(
            row_begin, row_end, places,
            inst->state_offset,
            inst->batch_arguments,
            arena);
    else
        inst->batch_that->addBatch(
            row_begin, row_end, places,
            inst->state_offset,
            inst->batch_arguments,
            arena);
}


void Aggregator::addBatchSinglePlace(
    size_t row_begin, size_t row_end,
    AggregateFunctionInstruction * inst,
    AggregateDataPtr place,
    Arena * arena)
{
    if (inst->offsets)
        inst->batch_that->addBatchSinglePlace(
            inst->offsets[static_cast<ssize_t>(row_begin) - 1],
            inst->offsets[row_end - 1],
            place,
            inst->batch_arguments,
            arena);
    else if (inst->has_sparse_arguments)
        inst->batch_that->addBatchSparseSinglePlace(
            row_begin, row_end, place,
            inst->batch_arguments,
            arena);
    else
        inst->batch_that->addBatchSinglePlace(
            row_begin, row_end, place,
            inst->batch_arguments,
            arena);
}


void NO_INLINE Aggregator::executeOnIntervalWithoutKey(
    AggregatedDataVariants & data_variants, size_t row_begin, size_t row_end, AggregateFunctionInstruction * aggregate_instructions) const
{
    /// `data_variants` will destroy the states of aggregate functions in the destructor
    data_variants.aggregator = this;
    data_variants.init(AggregatedDataVariants::Type::without_key);

    AggregatedDataWithoutKey & res = data_variants.without_key;

    /// Adding values
    for (AggregateFunctionInstruction * inst = aggregate_instructions; inst->that; ++inst)
    {
        if (inst->offsets)
            inst->batch_that->addBatchSinglePlace(
                inst->offsets[static_cast<ssize_t>(row_begin) - 1],
                inst->offsets[row_end - 1],
                res + inst->state_offset,
                inst->batch_arguments,
                data_variants.aggregates_pool);
        else
            inst->batch_that->addBatchSinglePlace(
                row_begin, row_end, res + inst->state_offset, inst->batch_arguments, data_variants.aggregates_pool);
    }
}

void NO_INLINE Aggregator::mergeOnIntervalWithoutKey(
    AggregatedDataVariants & data_variants,
    size_t row_begin,
    size_t row_end,
    const AggregateColumnsConstData & aggregate_columns_data,
    std::atomic<bool> & is_cancelled) const
{
    /// `data_variants` will destroy the states of aggregate functions in the destructor
    data_variants.aggregator = this;
    data_variants.init(AggregatedDataVariants::Type::without_key);

    mergeWithoutKeyStreamsImpl(data_variants, row_begin, row_end, aggregate_columns_data, is_cancelled);
}


void Aggregator::prepareAggregateInstructions(
    Columns columns,
    AggregateColumns & aggregate_columns,
    Columns & materialized_columns,
    AggregateFunctionInstructions & aggregate_functions_instructions,
    NestedColumnsHolder & nested_columns_holder) const
{
    for (size_t i = 0; i < params.aggregates_size; ++i)
        aggregate_columns[i].resize(params.aggregates[i].argument_names.size());

    aggregate_functions_instructions.resize(params.aggregates_size + 1);
    aggregate_functions_instructions[params.aggregates_size].that = nullptr;

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        bool allow_sparse_arguments = aggregate_columns[i].size() == 1;
        bool has_sparse_arguments = false;

        for (size_t j = 0; j < aggregate_columns[i].size(); ++j)
        {
            const auto pos = header.getPositionByName(params.aggregates[i].argument_names[j]);
            materialized_columns.push_back(columns.at(pos)->convertToFullColumnIfConst());
            aggregate_columns[i][j] = materialized_columns.back().get();

            /// Sparse columns without defaults may be handled incorrectly.
            if (aggregate_columns[i][j]->isSparse()
                && aggregate_columns[i][j]->getNumberOfDefaultRows() == 0)
                allow_sparse_arguments = false;

            auto full_column = allow_sparse_arguments
                ? aggregate_columns[i][j]->getPtr()
                : recursiveRemoveSparse(aggregate_columns[i][j]->getPtr());

            full_column = recursiveRemoveLowCardinality(full_column);
            if (full_column.get() != aggregate_columns[i][j])
            {
                materialized_columns.emplace_back(std::move(full_column));
                aggregate_columns[i][j] = materialized_columns.back().get();
            }

            if (aggregate_columns[i][j]->isSparse())
                has_sparse_arguments = true;
        }

        aggregate_functions_instructions[i].has_sparse_arguments = has_sparse_arguments;
        aggregate_functions_instructions[i].can_optimize_equal_keys_ranges = aggregate_functions[i]->canOptimizeEqualKeysRanges();
        aggregate_functions_instructions[i].arguments = aggregate_columns[i].data();
        aggregate_functions_instructions[i].state_offset = offsets_of_aggregate_states[i];

        const auto * that = aggregate_functions[i];
        /// Unnest consecutive trailing -State combinators
        while (const auto * func = typeid_cast<const AggregateFunctionState *>(that))
            that = func->getNestedFunction().get();
        aggregate_functions_instructions[i].that = that;

        if (const auto * func = typeid_cast<const AggregateFunctionArray *>(that))
        {
            /// Unnest consecutive -State combinators before -Array
            that = func->getNestedFunction().get();
            while (const auto * nested_func = typeid_cast<const AggregateFunctionState *>(that))
                that = nested_func->getNestedFunction().get();
            auto [nested_columns, offsets] = checkAndGetNestedArrayOffset(aggregate_columns[i].data(), that->getArgumentTypes().size());
            nested_columns_holder.push_back(std::move(nested_columns));
            aggregate_functions_instructions[i].batch_arguments = nested_columns_holder.back().data();
            aggregate_functions_instructions[i].offsets = offsets;
        }
        else
            aggregate_functions_instructions[i].batch_arguments = aggregate_columns[i].data();

        aggregate_functions_instructions[i].batch_that = that;
    }
}


bool Aggregator::executeOnBlock(const Block & block,
    AggregatedDataVariants & result,
    ColumnRawPtrs & key_columns,
    AggregateColumns & aggregate_columns,
    bool & no_more_keys) const
{
    return executeOnBlock(block.getColumns(),
        /* row_begin= */ 0, block.rows(),
        result,
        key_columns,
        aggregate_columns,
        no_more_keys);
}


bool Aggregator::executeOnBlock(Columns columns,
    size_t row_begin, size_t row_end,
    AggregatedDataVariants & result,
    ColumnRawPtrs & key_columns,
    AggregateColumns & aggregate_columns,
    bool & no_more_keys) const
{
    /// `result` will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// How to perform the aggregation?
    if (result.empty())
    {
        initDataVariantsWithSizeHint(result, method_chosen, params);
        result.keys_size = params.keys_size;
        result.key_sizes = key_sizes;
        LOG_TRACE(log, "Aggregation method: {}", result.getMethodName());
    }

    /** Constant columns are not supported directly during aggregation.
      * To make them work anyway, we materialize them.
      */
    Columns materialized_columns;
    bool all_keys_are_const = false;
    if (params.optimize_group_by_constant_keys)
    {
        all_keys_are_const = true;
        for (size_t i = 0; i < params.keys_size; ++i)
            all_keys_are_const &= isColumnConst(*columns.at(keys_positions[i]));
    }

    /// Remember the columns we will work with
    for (size_t i = 0; i < params.keys_size; ++i)
    {
        if (all_keys_are_const)
        {
            key_columns[i] = assert_cast<const ColumnConst &>(*columns.at(keys_positions[i])).getDataColumnPtr().get();
        }
        else
        {
            materialized_columns.push_back(recursiveRemoveSparse(columns.at(keys_positions[i]))->convertToFullColumnIfConst());
            key_columns[i] = materialized_columns.back().get();
        }


        if (!result.isLowCardinality())
        {
            auto column_no_lc = recursiveRemoveLowCardinality(key_columns[i]->getPtr());
            if (column_no_lc.get() != key_columns[i])
            {
                materialized_columns.emplace_back(std::move(column_no_lc));
                key_columns[i] = materialized_columns.back().get();
            }
        }
    }

    NestedColumnsHolder nested_columns_holder;
    AggregateFunctionInstructions aggregate_functions_instructions;
    prepareAggregateInstructions(columns, aggregate_columns, materialized_columns, aggregate_functions_instructions, nested_columns_holder);

    if ((params.overflow_row || result.type == AggregatedDataVariants::Type::without_key) && !result.without_key)
    {
        AggregateDataPtr place = result.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        result.without_key = place;
    }

    /// We select one of the aggregation methods and call it.

    /// For the case when there are no keys (all aggregate into one row).
    if (result.type == AggregatedDataVariants::Type::without_key)
    {
        /// TODO: Enable compilation after investigation
        bool use_compiled_functions = false;
        executeWithoutKeyImpl(
            result.without_key,
            row_begin,
            row_end,
            aggregate_functions_instructions.data(),
            result.aggregates_pool,
            use_compiled_functions);
    }
    else
    {
        /// This is where data is written that does not fit in `max_rows_to_group_by` with `group_by_overflow_mode = any`.
        AggregateDataPtr overflow_row_ptr = params.overflow_row ? result.without_key : nullptr;
        executeImpl(result, row_begin, row_end, key_columns, aggregate_functions_instructions.data(), no_more_keys, all_keys_are_const, overflow_row_ptr);
    }

    size_t result_size = result.sizeWithoutOverflowRow();
    Int64 current_memory_usage = getCurrentQueryMemoryUsage();

    /// Here all the results in the sum are taken into account, from different threads.
    auto result_size_bytes = current_memory_usage - memory_usage_before_aggregation;

    bool worth_convert_to_two_level = worthConvertToTwoLevel(
        params.group_by_two_level_threshold, result_size, params.group_by_two_level_threshold_bytes, result_size_bytes);

    /** Converting to a two-level data structure.
      * It allows you to make, in the subsequent, an effective merge - either economical from memory or parallel.
      */
    if (result.isConvertibleToTwoLevel() && worth_convert_to_two_level)
        result.convertToTwoLevel();

    /// Checking the constraints.
    if (!checkLimits(result_size, no_more_keys))
        return false;

    /** Flush data to disk if too much RAM is consumed.
      * Data can only be flushed to disk if a two-level aggregation structure is used.
      */
    if (params.max_bytes_before_external_group_by
        && result.isTwoLevel()
        && current_memory_usage > static_cast<Int64>(params.max_bytes_before_external_group_by)
        && worth_convert_to_two_level)
    {
        size_t size = current_memory_usage + params.min_free_disk_space;
        writeToTemporaryFile(result, size);
    }

    return true;
}


void Aggregator::writeToTemporaryFile(AggregatedDataVariants & data_variants, size_t max_temp_file_size) const
{
    if (!tmp_data)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot write to temporary file because temporary file is not initialized");

    Stopwatch watch;
    size_t rows = data_variants.size();

    auto & out_stream = [this, max_temp_file_size]() -> TemporaryBlockStreamHolder &
    {
        std::lock_guard lk(tmp_files_mutex);
        return tmp_files.emplace_back(getHeader(false), tmp_data.get(), max_temp_file_size);
    }();

    ProfileEvents::increment(ProfileEvents::ExternalAggregationWritePart);

    LOG_DEBUG(log, "Writing part of aggregation data into temporary file {}", out_stream.getHolder()->describeFilePath());

    /// Flush only two-level data and possibly overflow data.

#define M(NAME) \
    else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
        writeToTemporaryFileImpl(data_variants, *data_variants.NAME, out_stream);

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_TWO_LEVEL(M)
#undef M
    else
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant");

    /// NOTE Instead of freeing up memory and creating new hash tables and arenas, you can re-use the old ones.
    data_variants.init(data_variants.type);
    data_variants.aggregates_pools = Arenas(1, std::make_shared<Arena>());
    data_variants.aggregates_pool = data_variants.aggregates_pools.back().get();
    if (params.overflow_row || data_variants.type == AggregatedDataVariants::Type::without_key)
    {
        AggregateDataPtr place = data_variants.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        data_variants.without_key = place;
    }

    auto stat = out_stream.finishWriting();

    ProfileEvents::increment(ProfileEvents::ExternalAggregationCompressedBytes, stat.compressed_size);
    ProfileEvents::increment(ProfileEvents::ExternalAggregationUncompressedBytes, stat.uncompressed_size);
    ProfileEvents::increment(ProfileEvents::ExternalProcessingCompressedBytesTotal, stat.compressed_size);
    ProfileEvents::increment(ProfileEvents::ExternalProcessingUncompressedBytesTotal, stat.uncompressed_size);

    double elapsed_seconds = watch.elapsedSeconds();
    double compressed_size = stat.compressed_size;
    double uncompressed_size = stat.uncompressed_size;
    LOG_DEBUG(log,
        "Written part in {:.3f} sec., {} rows, {} uncompressed, {} compressed,"
        " {:.3f} uncompressed bytes per row, {:.3f} compressed bytes per row, compression rate: {:.3f}"
        " ({:.3f} rows/sec., {}/sec. uncompressed, {}/sec. compressed)",
        elapsed_seconds,
        rows,
        ReadableSize(uncompressed_size),
        ReadableSize(compressed_size),
        rows ? static_cast<double>(uncompressed_size) / rows : 0.0,
        rows ? static_cast<double>(compressed_size) / rows : 0.0,
        static_cast<double>(uncompressed_size) / compressed_size,
        static_cast<double>(rows) / elapsed_seconds,
        ReadableSize(static_cast<double>(uncompressed_size) / elapsed_seconds),
        ReadableSize(static_cast<double>(compressed_size) / elapsed_seconds));
}

template <typename Method>
Block Aggregator::convertOneBucketToBlock(
    AggregatedDataVariants & data_variants,
    Method & method,
    Arena * arena,
    bool final,
    Int32 bucket) const
{
    // Used in ConvertingAggregatedToChunksSource -> ConvertingAggregatedToChunksTransform (expects single chunk for each bucket_id).
    constexpr bool return_single_block = true;
    Block block = std::get<Block>(convertToBlockImpl(
        method,
        method.data.impls[bucket],
        arena,
        data_variants.aggregates_pools,
        final,
        method.data.impls[bucket].size(),
        return_single_block));

    block.info.bucket_num = static_cast<int>(bucket);
    return block;
}

Block Aggregator::mergeAndConvertOneBucketToBlock(
    ManyAggregatedDataVariants & variants,
    Arena * arena,
    bool final,
    Int32 bucket,
    std::atomic<bool> & is_cancelled) const
{
    auto & merged_data = *variants[0];
    auto method = merged_data.type;
    Block block;

    if (false) {} // NOLINT
#define M(NAME) \
    else if (method == AggregatedDataVariants::Type::NAME) \
    { \
        mergeBucketImpl<decltype(merged_data.NAME)::element_type>(variants, bucket, arena, is_cancelled); \
        if (is_cancelled.load(std::memory_order_seq_cst)) \
            return {}; \
        block = convertOneBucketToBlock(merged_data, *merged_data.NAME, arena, final, bucket); \
    }

    APPLY_FOR_VARIANTS_TWO_LEVEL(M)
#undef M

    return block;
}

Block Aggregator::convertOneBucketToBlock(AggregatedDataVariants & variants, Arena * arena, bool final, Int32 bucket) const
{
    const auto method = variants.type;
    Block block;

    if (false) {} // NOLINT
#define M(NAME) \
    else if (method == AggregatedDataVariants::Type::NAME) \
        block = convertOneBucketToBlock(variants, *variants.NAME, arena, final, bucket); \

    APPLY_FOR_VARIANTS_TWO_LEVEL(M)
#undef M

    return block;
}

std::list<TemporaryBlockStreamHolder> Aggregator::detachTemporaryData()
{
    std::lock_guard lk(tmp_files_mutex);
    return std::move(tmp_files);
}

bool Aggregator::hasTemporaryData() const
{
    std::lock_guard lk(tmp_files_mutex);
    return !tmp_files.empty();
}


template <typename Method>
void Aggregator::writeToTemporaryFileImpl(
    AggregatedDataVariants & data_variants,
    Method & method,
    TemporaryBlockStreamHolder & out) const
{
    size_t max_temporary_block_size_rows = 0;
    size_t max_temporary_block_size_bytes = 0;

    auto update_max_sizes = [&](const Block & block)
    {
        size_t block_size_rows = block.rows();
        size_t block_size_bytes = block.bytes();

        max_temporary_block_size_rows = std::max(block_size_rows, max_temporary_block_size_rows);
        max_temporary_block_size_bytes = std::max(block_size_bytes, max_temporary_block_size_bytes);
    };

    for (UInt32 bucket = 0; bucket < Method::Data::NUM_BUCKETS; ++bucket)
    {
        Block block = convertOneBucketToBlock(data_variants, method, data_variants.aggregates_pool, false, bucket);
        out->write(block);
        update_max_sizes(block);
    }

    if (params.overflow_row)
    {
        Block block = prepareBlockAndFillWithoutKey(data_variants, false, true);
        out->write(block);
        update_max_sizes(block);
    }

    /// Pass ownership of the aggregate functions states:
    /// `data_variants` will not destroy them in the destructor, they are now owned by ColumnAggregateFunction objects.
    data_variants.aggregator = nullptr;

    LOG_DEBUG(log, "Max size of temporary block: {} rows, {}.", max_temporary_block_size_rows, ReadableSize(max_temporary_block_size_bytes));
}


bool Aggregator::checkLimits(size_t result_size, bool & no_more_keys) const
{
    if (!no_more_keys && params.max_rows_to_group_by && result_size > params.max_rows_to_group_by)
    {
        switch (params.group_by_overflow_mode)
        {
            case OverflowMode::THROW:
                ProfileEvents::increment(ProfileEvents::OverflowThrow);
                throw Exception(ErrorCodes::TOO_MANY_ROWS, "Limit for rows to GROUP BY exceeded: has {} rows, maximum: {}",
                    result_size, params.max_rows_to_group_by);

            case OverflowMode::BREAK:
                ProfileEvents::increment(ProfileEvents::OverflowBreak);
                return false;

            case OverflowMode::ANY:
                ProfileEvents::increment(ProfileEvents::OverflowAny);
                no_more_keys = true;
                break;
        }
    }

    /// Some aggregate functions cannot throw exceptions on allocations (e.g. from C malloc)
    /// but still tracks memory. Check it here.
    CurrentMemoryTracker::check();
    return true;
}


template <typename Method, typename Table>
Aggregator::ConvertToBlockResVariant
Aggregator::convertToBlockImpl(Method & method, Table & data, Arena * arena, Arenas & aggregates_pools, bool final,size_t rows, bool return_single_block) const
{
    if (data.empty())
    {
        auto && out_cols = prepareOutputBlockColumns(params, aggregate_functions, getHeader(final), aggregates_pools, final, rows);
        auto finalized_block = finalizeBlock(params, getHeader(final), std::move(out_cols), final, rows);

        if (return_single_block)
            return std::move(finalized_block);

        return BlocksList{std::move(finalized_block)};
    }
    ConvertToBlockResVariant res;
    bool use_compiled_functions = false;
    if (final)
    {
#if USE_EMBEDDED_COMPILER
        use_compiled_functions = compiled_aggregate_functions_holder != nullptr && !Method::low_cardinality_optimization;
#endif
        res = convertToBlockImplFinal<Method>(method, data, arena, aggregates_pools, use_compiled_functions, return_single_block);
    }
    else
    {
        res = convertToBlockImplNotFinal(method, data, aggregates_pools, rows, return_single_block);
    }

    /// In order to release memory early.
    data.clearAndShrink();

    return res;
}


template <typename Mapped>
inline void Aggregator::insertAggregatesIntoColumns(Mapped & mapped, MutableColumns & final_aggregate_columns, Arena * arena) const
{
    /** Final values of aggregate functions are inserted to columns.
      * Then states of aggregate functions, that are not longer needed, are destroyed.
      *
      * We mark already destroyed states with "nullptr" in data,
      *  so they will not be destroyed in destructor of Aggregator
      * (other values will be destroyed in destructor in case of exception).
      *
      * But it becomes tricky, because we have multiple aggregate states pointed by a single pointer in data.
      * So, if exception is thrown in the middle of moving states for different aggregate functions,
      *  we have to catch exceptions and destroy all the states that are no longer needed,
      *  to keep the data in consistent state.
      *
      * It is also tricky, because there are aggregate functions with "-State" modifier.
      * When we call "insertResultInto" for them, they insert a pointer to the state to ColumnAggregateFunction
      *  and ColumnAggregateFunction will take ownership of this state.
      * So, for aggregate functions with "-State" modifier, only states of all combinators that are used
      *  after -State will be destroyed after result has been transferred to ColumnAggregateFunction.
      *  For example, if we have function `uniqStateForEachMap` after aggregation we should destroy all states that
      *  were created by combinators `-ForEach` and `-Map`, because resulting ColumnAggregateFunction will be
      *  responsible only for destruction of the states created by `uniq` function.
      * But we should mark that the data no longer owns these states.
      */

    size_t insert_i = 0;
    std::exception_ptr exception;

    try
    {
        /// Insert final values of aggregate functions into columns.
        for (; insert_i < params.aggregates_size; ++insert_i)
            aggregate_functions[insert_i]->insertResultInto(
                mapped + offsets_of_aggregate_states[insert_i],
                *final_aggregate_columns[insert_i],
                arena);
    }
    catch (...)
    {
        exception = std::current_exception();
    }

    /** Destroy states that are no longer needed. This loop does not throw.
        *
        * For functions with -State combinator we destroy only states of all combinators that are used
        *  after -State, because the ownership of the rest states is transferred to ColumnAggregateFunction
        *  and ColumnAggregateFunction will take care.
        *
        * But it's only for states that has been transferred to ColumnAggregateFunction
        *  before exception has been thrown;
        */
    for (size_t destroy_i = 0; destroy_i < params.aggregates_size; ++destroy_i)
    {
        if (destroy_i < insert_i)
            aggregate_functions[destroy_i]->destroyUpToState(mapped + offsets_of_aggregate_states[destroy_i]);
        else
            aggregate_functions[destroy_i]->destroy(mapped + offsets_of_aggregate_states[destroy_i]);
    }

    /// Mark the cell as destroyed so it will not be destroyed in destructor.
    mapped = nullptr;

    if (exception)
        std::rethrow_exception(exception);
}


Block Aggregator::insertResultsIntoColumns(
    PaddedPODArray<AggregateDataPtr> & places,
    OutputBlockColumns && out_cols,
    Arena * arena,
    bool has_null_key_data [[maybe_unused]],
    bool use_compiled_functions [[maybe_unused]]) const
{
    std::exception_ptr exception;
    size_t aggregate_functions_destroy_index = 0;

    try
    {
#if USE_EMBEDDED_COMPILER
        if (use_compiled_functions)
        {
            /** For JIT compiled functions we need to resize columns before pass them into compiled code.
              * insert_aggregates_into_columns_function function does not throw exception.
              */
            std::vector<ColumnData> columns_data;

            auto compiled_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;

            for (size_t i = 0; i < params.aggregates_size; ++i)
            {
                if (!is_aggregate_function_compiled[i])
                    continue;

                auto & final_aggregate_column = out_cols.final_aggregate_columns[i];
                /**
                 * In convertToBlockImplFinal, additional data with a key of null may be written,
                 * and additional memory for null data needs to be allocated when using the compiled function
                 */
                final_aggregate_column = final_aggregate_column->cloneResized(places.size() + (has_null_key_data ? 1 : 0));
                columns_data.emplace_back(getColumnData(final_aggregate_column.get(), (has_null_key_data ? 1 : 0)));
            }

            auto insert_aggregates_into_columns_function = compiled_functions.insert_aggregates_into_columns_function;
            insert_aggregates_into_columns_function(0, places.size(), columns_data.data(), places.data());
        }
#endif

        for (; aggregate_functions_destroy_index < params.aggregates_size;)
        {
#if USE_EMBEDDED_COMPILER
            if (use_compiled_functions && is_aggregate_function_compiled[aggregate_functions_destroy_index])
            {
                ++aggregate_functions_destroy_index;
                continue;
            }
#endif

            auto & final_aggregate_column = out_cols.final_aggregate_columns[aggregate_functions_destroy_index];
            size_t offset = offsets_of_aggregate_states[aggregate_functions_destroy_index];

            /** We increase aggregate_functions_destroy_index because by function contract if insertResultIntoBatch
              * throws exception, it also must destroy all necessary states.
              * Then code need to continue to destroy other aggregate function states with next function index.
              */
            size_t destroy_index = aggregate_functions_destroy_index;
            ++aggregate_functions_destroy_index;

            aggregate_functions[destroy_index]->insertResultIntoBatch(0, places.size(), places.data(), offset, *final_aggregate_column, arena);
        }
    }
    catch (...)
    {
        exception = std::current_exception();
    }

    for (; aggregate_functions_destroy_index < params.aggregates_size; ++aggregate_functions_destroy_index)
    {
#if USE_EMBEDDED_COMPILER
        if (use_compiled_functions && is_aggregate_function_compiled[aggregate_functions_destroy_index])
        {
            ++aggregate_functions_destroy_index;
            continue;
        }
#endif

        size_t offset = offsets_of_aggregate_states[aggregate_functions_destroy_index];
        aggregate_functions[aggregate_functions_destroy_index]->destroyBatch(0, places.size(), places.data(), offset);
    }

    if (exception)
        std::rethrow_exception(exception);

    return finalizeBlock(params, getHeader(/* final */ true), std::move(out_cols), /* final */ true, places.size());
}

template <typename Method, typename Table>
Aggregator::ConvertToBlockResVariant Aggregator::convertToBlockImplFinal(
    Method & method,
    Table & data,
    Arena * arena,
    Arenas & aggregates_pools,
    bool use_compiled_functions [[maybe_unused]],
    bool return_single_block) const
{
    /// +1 for nullKeyData, if `data` doesn't have it - not a problem, just some memory for one excessive row will be preallocated
    const size_t max_block_size = (return_single_block ? data.size() : std::min(params.max_block_size, data.size())) + 1;
    const bool final = true;

    std::optional<OutputBlockColumns> out_cols;
    std::optional<Sizes> shuffled_key_sizes;
    PaddedPODArray<AggregateDataPtr> places;
    bool has_null_key_data = false;
    BlocksList blocks;

    auto init_out_cols = [&]()
    {
        out_cols = prepareOutputBlockColumns(params, aggregate_functions, getHeader(final), aggregates_pools, final, max_block_size);

        if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
        {
            /**
             * When one_key_nullable_optimization is enabled, null data will be written to the key column and result column in advance.
             * And in insertResultsIntoColumns need to allocate memory for null data.
             */
            if (data.hasNullKeyData())
            {
                has_null_key_data = true;
                out_cols->key_columns[0]->insertDefault();
                insertAggregatesIntoColumns(data.getNullKeyData(), out_cols->final_aggregate_columns, arena);
                data.hasNullKeyData() = false;
            }
        }

        shuffled_key_sizes = method.shuffleKeyColumns(out_cols->raw_key_columns, key_sizes);

        places.reserve(max_block_size);
    };

    // should be invoked at least once, because null data might be the only content of the `data`
    init_out_cols();

    data.forEachValue(
        [&](const auto & key, auto & mapped)
        {
            if (unlikely(!out_cols.has_value()))
                init_out_cols();

            const auto & key_sizes_ref = shuffled_key_sizes ? *shuffled_key_sizes : key_sizes;
            method.insertKeyIntoColumns(key, out_cols->raw_key_columns, key_sizes_ref);
            places.emplace_back(mapped);

            /// Mark the cell as destroyed so it will not be destroyed in destructor.
            mapped = nullptr;

            if (!return_single_block && places.size() >= max_block_size)
            {
                blocks.emplace_back(
                    insertResultsIntoColumns(places, std::move(out_cols.value()), arena, has_null_key_data, use_compiled_functions));
                places.clear();
                out_cols.reset();
                has_null_key_data = false;
            }
        });

    if (return_single_block)
    {
        return insertResultsIntoColumns(places, std::move(out_cols.value()), arena, has_null_key_data, use_compiled_functions);
    }

    if (out_cols.has_value())
    {
        blocks.emplace_back(
            insertResultsIntoColumns(places, std::move(out_cols.value()), arena, has_null_key_data, use_compiled_functions));
    }
    return blocks;
}

template <typename Method, typename Table>
Aggregator::ConvertToBlockResVariant NO_INLINE
Aggregator::convertToBlockImplNotFinal(Method & method, Table & data, Arenas & aggregates_pools, size_t, bool return_single_block) const
{
    /// +1 for nullKeyData, if `data` doesn't have it - not a problem, just some memory for one excessive row will be preallocated
    const size_t max_block_size = (return_single_block ? data.size() : std::min(params.max_block_size, data.size())) + 1;
    const bool final = false;
    BlocksList res_blocks;

    std::optional<OutputBlockColumns> out_cols;
    std::optional<Sizes> shuffled_key_sizes;
    size_t rows_in_current_block = 0;

    auto init_out_cols = [&]()
    {
        out_cols = prepareOutputBlockColumns(params, aggregate_functions, getHeader(final), aggregates_pools, final, max_block_size);

        if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
        {
            if (data.hasNullKeyData())
            {
                out_cols->raw_key_columns[0]->insertDefault();

                for (size_t i = 0; i < params.aggregates_size; ++i)
                    out_cols->aggregate_columns_data[i]->push_back(data.getNullKeyData() + offsets_of_aggregate_states[i]);

                ++rows_in_current_block;
                data.getNullKeyData() = nullptr;
                data.hasNullKeyData() = false;
            }
        }

        shuffled_key_sizes = method.shuffleKeyColumns(out_cols->raw_key_columns, key_sizes);
    };

    // should be invoked at least once, because null data might be the only content of the `data`
    init_out_cols();
    data.forEachValue(
        [&](const auto & key, auto & mapped)
        {
            if (!out_cols.has_value())
                init_out_cols();

            const auto & key_sizes_ref = shuffled_key_sizes ? *shuffled_key_sizes : key_sizes;
            method.insertKeyIntoColumns(key, out_cols->raw_key_columns, key_sizes_ref);

            /// reserved, so push_back does not throw exceptions
            for (size_t i = 0; i < params.aggregates_size; ++i)
                out_cols->aggregate_columns_data[i]->push_back(mapped + offsets_of_aggregate_states[i]);

            mapped = nullptr;

            ++rows_in_current_block;
            if (!return_single_block && rows_in_current_block >= max_block_size)
            {
                res_blocks.emplace_back(finalizeBlock(params, getHeader(final), std::move(out_cols.value()), final, rows_in_current_block));
                out_cols.reset();
                rows_in_current_block = 0;
            }
        });

    if (return_single_block)
    {
        return finalizeBlock(params, getHeader(final), std::move(out_cols).value(), final, rows_in_current_block);
    }

    if (rows_in_current_block)
        res_blocks.emplace_back(finalizeBlock(params, getHeader(final), std::move(out_cols).value(), final, rows_in_current_block));
    return res_blocks;
}

void Aggregator::addSingleKeyToAggregateColumns(
    AggregatedDataVariants & data_variants,
    MutableColumns & aggregate_columns) const
{
    auto & data = data_variants.without_key;

    size_t i = 0;
    try
    {
        for (i = 0; i < params.aggregates_size; ++i)
        {
            auto & column_aggregate_func = assert_cast<ColumnAggregateFunction &>(*aggregate_columns[i]);
            column_aggregate_func.getData().push_back(data + offsets_of_aggregate_states[i]);
        }
    }
    catch (...)
    {
        /// Rollback
        for (size_t rollback_i = 0; rollback_i < i; ++rollback_i)
        {
            auto & column_aggregate_func = assert_cast<ColumnAggregateFunction &>(*aggregate_columns[rollback_i]);
            column_aggregate_func.getData().pop_back();
        }
        throw;
    }

    data = nullptr;
}

void Aggregator::addArenasToAggregateColumns(
    const AggregatedDataVariants & data_variants,
    MutableColumns & aggregate_columns) const
{
    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        auto & column_aggregate_func = assert_cast<ColumnAggregateFunction &>(*aggregate_columns[i]);
        for (const auto & pool : data_variants.aggregates_pools)
            column_aggregate_func.addArena(pool);
    }
}

void Aggregator::createStatesAndFillKeyColumnsWithSingleKey(
    AggregatedDataVariants & data_variants,
    Columns & key_columns,
    size_t key_row,
    MutableColumns & final_key_columns) const
{
    AggregateDataPtr place = data_variants.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
    createAggregateStates(place);
    data_variants.without_key = place;

    for (size_t i = 0; i < params.keys_size; ++i)
    {
        final_key_columns[i]->insertFrom(*key_columns[i].get(), key_row);
    }
}

Block Aggregator::prepareBlockAndFillWithoutKey(AggregatedDataVariants & data_variants, bool final, bool is_overflows) const
{
    size_t rows = 1;
    auto && out_cols
        = prepareOutputBlockColumns(params, aggregate_functions, getHeader(final), data_variants.aggregates_pools, final, rows);
    auto && [key_columns, raw_key_columns, aggregate_columns, final_aggregate_columns, aggregate_columns_data] = out_cols;

    if (data_variants.type == AggregatedDataVariants::Type::without_key || params.overflow_row)
    {
        AggregatedDataWithoutKey & data = data_variants.without_key;

        if (!data)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Wrong data variant passed.");

        if (!final)
        {
            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_columns_data[i]->push_back(data + offsets_of_aggregate_states[i]);
            data = nullptr;
        }
        else
        {
            /// Always single-thread. It's safe to pass current arena from 'aggregates_pool'.
            insertAggregatesIntoColumns(data, final_aggregate_columns, data_variants.aggregates_pool);
        }

        if (params.overflow_row)
            for (size_t i = 0; i < params.keys_size; ++i)
                key_columns[i]->insertDefault();
    }

    Block block = finalizeBlock(params, getHeader(final), std::move(out_cols), final, rows);

    if (is_overflows)
        block.info.is_overflows = true;

    if (final)
        destroyWithoutKey(data_variants);

    return block;
}

template <bool return_single_block>
Aggregator::ConvertToBlockRes<return_single_block>
Aggregator::prepareBlockAndFillSingleLevel(AggregatedDataVariants & data_variants, bool final) const
{
    ConvertToBlockResVariant res_variant;
    const size_t rows = data_variants.sizeWithoutOverflowRow();
#define M(NAME) \
    else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
    { \
        res_variant = convertToBlockImpl( \
            *data_variants.NAME, data_variants.NAME->data, data_variants.aggregates_pool, data_variants.aggregates_pools, final, rows, return_single_block); \
    }

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_SINGLE_LEVEL(M)
#undef M
    else throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");
    if constexpr (return_single_block)
        return std::get<Block>(res_variant);
    else
        return std::get<BlocksList>(res_variant);
}


BlocksList Aggregator::prepareBlocksAndFillTwoLevel(AggregatedDataVariants & data_variants, bool final) const
{
#define M(NAME) \
    else if (data_variants.type == AggregatedDataVariants::Type::NAME) \
        return prepareBlocksAndFillTwoLevelImpl(data_variants, *data_variants.NAME, final);

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_TWO_LEVEL(M)
#undef M
    else
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");
}


template <typename Method>
BlocksList Aggregator::prepareBlocksAndFillTwoLevelImpl(AggregatedDataVariants & data_variants, Method & method, bool final) const
{
    /// TODO Make a custom threshold.
    const bool use_thread_pool = params.max_threads > 1 && data_variants.sizeWithoutOverflowRow() > 100000 && data_variants.isTwoLevel();
    const size_t max_threads = use_thread_pool ? params.max_threads : 1;
    if (max_threads > data_variants.aggregates_pools.size())
        for (size_t i = data_variants.aggregates_pools.size(); i < max_threads; ++i)
            data_variants.aggregates_pools.push_back(std::make_shared<Arena>());

    std::atomic<UInt32> next_bucket_to_merge = 0;
    std::vector<BlocksList> res(max_threads);

    auto converter = [&](size_t thread_id)
    {
        while (true)
        {
            UInt32 bucket = next_bucket_to_merge.fetch_add(1);

            if (bucket >= Method::Data::NUM_BUCKETS)
                break;

            if (method.data.impls[bucket].empty())
                continue;

            /// Select Arena to avoid race conditions
            Arena * arena = data_variants.aggregates_pools.at(thread_id).get();
            res[thread_id].emplace_back(convertOneBucketToBlock(data_variants, method, arena, final, bucket));
        }
    };

    ThreadPoolCallbackRunnerLocal<void> runner(thread_pool, "AggregatorPool");
    try
    {
        for (size_t thread_id = 0; thread_id < max_threads; ++thread_id)
        {
            if (use_thread_pool)
                runner([&converter, thread_id]() { return converter(thread_id); }, Priority{});
            else
                converter(thread_id);
        }
    }
    catch (...)
    {
        runner.waitForAllToFinishAndRethrowFirstError();
    }

    runner.waitForAllToFinishAndRethrowFirstError();

    BlocksList blocks;
    for (auto & blocks_list : res)
        blocks.splice(blocks.end(), std::move(blocks_list));
    return blocks;
}


BlocksList Aggregator::convertToBlocks(AggregatedDataVariants & data_variants, bool final) const
{
    LOG_TRACE(log, "Converting aggregated data to blocks");

    Stopwatch watch;

    BlocksList blocks;

    /// In what data structure is the data aggregated?
    if (data_variants.empty())
        return blocks;

    if (data_variants.without_key)
        blocks.emplace_back(prepareBlockAndFillWithoutKey(
            data_variants, final, data_variants.type != AggregatedDataVariants::Type::without_key));

    if (data_variants.type != AggregatedDataVariants::Type::without_key)
    {
        if (!data_variants.isTwoLevel())
            blocks.splice(blocks.end(), prepareBlockAndFillSingleLevel<false>(data_variants, final));
        else
            blocks.splice(blocks.end(), prepareBlocksAndFillTwoLevel(data_variants, final));
    }

    if (!final)
    {
        /// data_variants will not destroy the states of aggregate functions in the destructor.
        /// Now ColumnAggregateFunction owns the states.
        data_variants.aggregator = nullptr;
    }

    size_t rows = 0;
    size_t bytes = 0;

    for (const auto & block : blocks)
    {
        rows += block.rows();
        bytes += block.bytes();
    }

    double elapsed_seconds = watch.elapsedSeconds();
    LOG_DEBUG(log,
        "Converted aggregated data to blocks. {} rows, {} in {} sec. ({:.3f} rows/sec., {}/sec.)",
        rows, ReadableSize(bytes),
        elapsed_seconds, rows / elapsed_seconds,
        ReadableSize(bytes / elapsed_seconds));

    return blocks;
}


template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataNullKey(
    Table & table_dst,
    Table & table_src,
    Arena * arena) const
{
    if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
    {
        if (table_src.hasNullKeyData())
        {
            if (!table_dst.hasNullKeyData())
            {
                table_dst.hasNullKeyData() = true;
                table_dst.getNullKeyData() = table_src.getNullKeyData();
            }
            else
            {
                for (size_t i = 0; i < params.aggregates_size; ++i)
                    aggregate_functions[i]->merge(
                            table_dst.getNullKeyData() + offsets_of_aggregate_states[i],
                            table_src.getNullKeyData() + offsets_of_aggregate_states[i],
                            arena);

                for (size_t i = 0; i < params.aggregates_size; ++i)
                    aggregate_functions[i]->destroy(
                            table_src.getNullKeyData() + offsets_of_aggregate_states[i]);
            }

            table_src.hasNullKeyData() = false;
            table_src.getNullKeyData() = nullptr;
        }
    }
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataImpl(
    Table & table_dst, Table & table_src, Arena * arena, bool use_compiled_functions [[maybe_unused]], bool prefetch, std::atomic<bool> & is_cancelled) const
{
    if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
        mergeDataNullKey<Method, Table>(table_dst, table_src, arena);

    PaddedPODArray<AggregateDataPtr> dst_places;
    PaddedPODArray<AggregateDataPtr> src_places;

    auto merge = [&](AggregateDataPtr & __restrict dst, AggregateDataPtr & __restrict src, bool inserted)
    {
        if (!inserted)
        {
            dst_places.push_back(dst);
            src_places.push_back(src);
        }
        else
        {
            dst = src;
        }

        src = nullptr;
    };

    if (prefetch)
        table_src.template mergeToViaEmplace<decltype(merge), true>(table_dst, std::move(merge));
    else
        table_src.template mergeToViaEmplace<decltype(merge), false>(table_dst, std::move(merge));
    table_src.clearAndShrink();

#if USE_EMBEDDED_COMPILER
    if (use_compiled_functions)
    {
        const auto & compiled_functions = compiled_aggregate_functions_holder->compiled_aggregate_functions;
        compiled_functions.merge_aggregate_states_function(dst_places.data(), src_places.data(), dst_places.size());

        for (size_t i = 0; i < params.aggregates_size; ++i)
        {
            if (!is_aggregate_function_compiled[i])
                aggregate_functions[i]->mergeAndDestroyBatch(
                    dst_places.data(), src_places.data(), dst_places.size(), offsets_of_aggregate_states[i], thread_pool, is_cancelled, arena);
        }

        return;
    }
#endif

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        aggregate_functions[i]->mergeAndDestroyBatch(
            dst_places.data(), src_places.data(), dst_places.size(), offsets_of_aggregate_states[i], thread_pool, is_cancelled, arena);
    }
}


template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataNoMoreKeysImpl(
    Table & table_dst,
    AggregatedDataWithoutKey & overflows,
    Table & table_src,
    Arena * arena) const
{
    /// Note : will create data for NULL key if not exist
    if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
        mergeDataNullKey<Method, Table>(table_dst, table_src, arena);

    table_src.mergeToViaFind(table_dst, [&](AggregateDataPtr dst, AggregateDataPtr & src, bool found)
    {
        AggregateDataPtr res_data = found ? dst : overflows;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->merge(
                res_data + offsets_of_aggregate_states[i],
                src + offsets_of_aggregate_states[i],
                arena);

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(src + offsets_of_aggregate_states[i]);

        src = nullptr;
    });
    table_src.clearAndShrink();
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeDataOnlyExistingKeysImpl(
    Table & table_dst,
    Table & table_src,
    Arena * arena) const
{
    /// Note : will create data for NULL key if not exist
    if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
        mergeDataNullKey<Method, Table>(table_dst, table_src, arena);

    table_src.mergeToViaFind(table_dst,
        [&](AggregateDataPtr dst, AggregateDataPtr & src, bool found)
    {
        if (!found)
            return;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->merge(
                dst + offsets_of_aggregate_states[i],
                src + offsets_of_aggregate_states[i],
                arena);

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(src + offsets_of_aggregate_states[i]);

        src = nullptr;
    });
    table_src.clearAndShrink();
}


void NO_INLINE Aggregator::mergeWithoutKeyDataImpl(
    ManyAggregatedDataVariants & non_empty_data,
    std::atomic<bool> & is_cancelled) const
{
    AggregatedDataVariantsPtr & res = non_empty_data[0];

    for (size_t i = 0; i < params.aggregates_size; ++i)
    {
        if (aggregate_functions[i]->isParallelizeMergePrepareNeeded())
        {
            size_t size = non_empty_data.size();
            std::vector<AggregateDataPtr> data_vec;
            data_vec.reserve(size);

            for (size_t result_num = 0; result_num < size; ++result_num)
                data_vec.emplace_back(non_empty_data[result_num]->without_key + offsets_of_aggregate_states[i]);

            aggregate_functions[i]->parallelizeMergePrepare(data_vec, thread_pool, is_cancelled);
        }
    }

    /// We merge all aggregation results to the first.
    for (size_t result_num = 1, size = non_empty_data.size(); result_num < size; ++result_num)
    {
        AggregatedDataWithoutKey & res_data = res->without_key;
        AggregatedDataWithoutKey & current_data = non_empty_data[result_num]->without_key;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            if (aggregate_functions[i]->isAbleToParallelizeMerge())
                aggregate_functions[i]->merge(
                    res_data + offsets_of_aggregate_states[i],
                    current_data + offsets_of_aggregate_states[i],
                    thread_pool,
                    is_cancelled,
                    res->aggregates_pool);
            else
                aggregate_functions[i]->merge(
                    res_data + offsets_of_aggregate_states[i], current_data + offsets_of_aggregate_states[i], res->aggregates_pool);

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(current_data + offsets_of_aggregate_states[i]);

        current_data = nullptr;
    }
}


template <typename Method>
void NO_INLINE Aggregator::mergeSingleLevelDataImpl(
    ManyAggregatedDataVariants & non_empty_data, std::atomic<bool> & is_cancelled) const
{
    AggregatedDataVariantsPtr & res = non_empty_data[0];
    bool no_more_keys = false;

    const bool prefetch = Method::State::has_cheap_key_calculation && params.enable_prefetch
        && (getDataVariant<Method>(*res).data.getBufferSizeInBytes() > min_bytes_for_prefetch);

    /// We merge all aggregation results to the first.
    for (size_t result_num = 1, size = non_empty_data.size(); result_num < size; ++result_num)
    {
        if (!checkLimits(res->sizeWithoutOverflowRow(), no_more_keys))
            break;

        AggregatedDataVariants & current = *non_empty_data[result_num];

        if (!no_more_keys)
        {
#if USE_EMBEDDED_COMPILER
            if (compiled_aggregate_functions_holder)
            {
                mergeDataImpl<Method>(
                    getDataVariant<Method>(*res).data, getDataVariant<Method>(current).data, res->aggregates_pool, true, prefetch, is_cancelled);
            }
            else
#endif
            {
                mergeDataImpl<Method>(
                    getDataVariant<Method>(*res).data, getDataVariant<Method>(current).data, res->aggregates_pool, false, prefetch, is_cancelled);
            }
        }
        else if (res->without_key)
        {
            mergeDataNoMoreKeysImpl<Method>(
                getDataVariant<Method>(*res).data,
                res->without_key,
                getDataVariant<Method>(current).data,
                res->aggregates_pool);
        }
        else
        {
            mergeDataOnlyExistingKeysImpl<Method>(
                getDataVariant<Method>(*res).data,
                getDataVariant<Method>(current).data,
                res->aggregates_pool);
        }

        /// `current` will not destroy the states of aggregate functions in the destructor
        current.aggregator = nullptr;
    }
}

#define M(NAME) \
    template void NO_INLINE Aggregator::mergeSingleLevelDataImpl<decltype(AggregatedDataVariants::NAME)::element_type>( \
        ManyAggregatedDataVariants & non_empty_data, std::atomic<bool> & is_cancelled) const;
    APPLY_FOR_VARIANTS_SINGLE_LEVEL(M)
#undef M

template <typename Method>
void NO_INLINE Aggregator::mergeBucketImpl(
    ManyAggregatedDataVariants & data, Int32 bucket, Arena * arena, std::atomic<bool> & is_cancelled) const
{
    /// We merge all aggregation results to the first.
    AggregatedDataVariantsPtr & res = data[0];

    const bool prefetch = Method::State::has_cheap_key_calculation && params.enable_prefetch
        && (Method::Data::NUM_BUCKETS * getDataVariant<Method>(*res).data.impls[bucket].getBufferSizeInBytes() > min_bytes_for_prefetch);

    for (size_t result_num = 1, size = data.size(); result_num < size; ++result_num)
    {
        if (is_cancelled.load(std::memory_order_seq_cst))
            return;

        AggregatedDataVariants & current = *data[result_num];
#if USE_EMBEDDED_COMPILER
        if (compiled_aggregate_functions_holder)
        {
            mergeDataImpl<Method>(
                getDataVariant<Method>(*res).data.impls[bucket], getDataVariant<Method>(current).data.impls[bucket], arena, true, prefetch, is_cancelled);
        }
        else
#endif
        {
            mergeDataImpl<Method>(
                getDataVariant<Method>(*res).data.impls[bucket],
                getDataVariant<Method>(current).data.impls[bucket],
                arena,
                false,
                prefetch,
                is_cancelled);
        }
    }
}

ManyAggregatedDataVariants Aggregator::prepareVariantsToMerge(ManyAggregatedDataVariants && data_variants) const
{
    if (data_variants.empty())
        throw Exception(ErrorCodes::EMPTY_DATA_PASSED, "Empty data passed to Aggregator::prepareVariantsToMerge.");

    LOG_TRACE(log, "Merging aggregated data");

    updateStatistics(data_variants, params.stats_collecting_params);

    ManyAggregatedDataVariants non_empty_data;
    non_empty_data.reserve(data_variants.size());
    for (auto & data : data_variants)
        if (!data->empty())
            non_empty_data.push_back(data);

    if (non_empty_data.empty())
        return {};

    if (non_empty_data.size() > 1)
    {
        /// Sort the states in descending order so that the merge is more efficient (since all states are merged into the first).
        ::sort(non_empty_data.begin(), non_empty_data.end(),
            [](const AggregatedDataVariantsPtr & lhs, const AggregatedDataVariantsPtr & rhs)
            {
                return lhs->sizeWithoutOverflowRow() > rhs->sizeWithoutOverflowRow();
            });
    }

    /// If at least one of the options is two-level, then convert all the options into two-level ones, if there are not such.
    /// Note - perhaps it would be more optimal not to convert single-level versions before the merge, but merge them separately, at the end.

    bool has_at_least_one_two_level = false;
    for (const auto & variant : non_empty_data)
    {
        if (variant->isTwoLevel())
        {
            has_at_least_one_two_level = true;
            break;
        }
    }

    if (has_at_least_one_two_level)
        for (auto & variant : non_empty_data)
            if (!variant->isTwoLevel())
                variant->convertToTwoLevel();

    AggregatedDataVariantsPtr & first = non_empty_data[0];

    for (size_t i = 1, size = non_empty_data.size(); i < size; ++i)
    {
        if (first->type != non_empty_data[i]->type)
            throw Exception(ErrorCodes::CANNOT_MERGE_DIFFERENT_AGGREGATED_DATA_VARIANTS, "Cannot merge different aggregated data variants.");

        /** Elements from the remaining sets can be moved to the first data set.
          * Therefore, it must own all the arenas of all other sets.
          */
        first->aggregates_pools.insert(first->aggregates_pools.end(),
            non_empty_data[i]->aggregates_pools.begin(), non_empty_data[i]->aggregates_pools.end());
    }

    return non_empty_data;
}

template <typename State, typename Table>
void NO_INLINE Aggregator::mergeStreamsImplCase(
    Arena * aggregates_pool,
    State & state,
    Table & data,
    bool no_more_keys,
    AggregateDataPtr overflow_row,
    size_t row_begin,
    size_t row_end,
    const AggregateColumnsConstData & aggregate_columns_data,
    std::atomic<bool> & is_cancelled,
    Arena * arena_for_keys) const
{
    std::unique_ptr<AggregateDataPtr[]> places(new AggregateDataPtr[row_end]);

    if (!arena_for_keys)
        arena_for_keys = aggregates_pool;

    if (no_more_keys)
    {
        for (size_t i = row_begin; i < row_end; i++)
        {
            auto find_result = state.findKey(data, i, *arena_for_keys);
            /// aggregate_date == nullptr means that the new key did not fit in the hash table because of no_more_keys.
            AggregateDataPtr value = find_result.isFound() ? find_result.getMapped() : overflow_row;
            places[i] = value;
        }
    }
    else
    {
        for (size_t i = row_begin; i < row_end; i++)
        {
            /// clang-tidy complains wrongly about this one when running the analysis from an ARM host.
            /// The same thing does not fail when cross-compiling from a x86_64 host.
            /// Furthermore, arena_for_keys is set to be a pointer to the last member of aggregates_pools,
            /// which is always initialized to have at least 1 arena.
            auto emplace_result = state.emplaceKey(data, i, *arena_for_keys); /// NOLINT(clang-analyzer-core.NonNullParamChecker)
            if (!emplace_result.isInserted())
                places[i] = emplace_result.getMapped();
            else
            {
                emplace_result.setMapped(nullptr);

                AggregateDataPtr aggregate_data = aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
                createAggregateStates(aggregate_data);

                emplace_result.setMapped(aggregate_data);
                places[i] = aggregate_data;
            }
        }
    }

    for (size_t j = 0; j < params.aggregates_size; ++j)
    {
        /// Merge state of aggregate functions.
        aggregate_functions[j]->mergeBatch(
            row_begin,
            row_end,
            places.get(),
            offsets_of_aggregate_states[j],
            aggregate_columns_data[j]->data(),
            thread_pool,
            is_cancelled,
            aggregates_pool);
    }
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeStreamsImpl(
    Block block,
    Arena * aggregates_pool,
    Method & method,
    Table & data,
    AggregateDataPtr overflow_row,
    LastElementCacheStats & consecutive_keys_cache_stats,
    bool no_more_keys,
    std::atomic<bool> & is_cancelled,
    Arena * arena_for_keys) const
{
    const AggregateColumnsConstData & aggregate_columns_data = params.makeAggregateColumnsData(block);
    ColumnRawPtrs key_columns = params.makeRawKeyColumns(block);

    mergeStreamsImpl<Method, Table>(
        aggregates_pool,
        method,
        data,
        overflow_row,
        consecutive_keys_cache_stats,
        no_more_keys,
        0,
        block.rows(),
        aggregate_columns_data,
        key_columns,
        is_cancelled,
        arena_for_keys);
}

template <typename Method, typename Table>
void NO_INLINE Aggregator::mergeStreamsImpl(
    Arena * aggregates_pool,
    Method & method [[maybe_unused]],
    Table & data,
    AggregateDataPtr overflow_row,
    LastElementCacheStats & consecutive_keys_cache_stats,
    bool no_more_keys,
    size_t row_begin,
    size_t row_end,
    const AggregateColumnsConstData & aggregate_columns_data,
    const ColumnRawPtrs & key_columns,
    std::atomic<bool> & is_cancelled,
    Arena * arena_for_keys) const
{
    UInt64 total_rows = consecutive_keys_cache_stats.hits + consecutive_keys_cache_stats.misses;
    double cache_hit_rate = total_rows ? static_cast<double>(consecutive_keys_cache_stats.hits) / total_rows : 1.0;
    bool use_cache = cache_hit_rate >= params.min_hit_rate_to_use_consecutive_keys_optimization;

    if (use_cache)
    {
        typename Method::State state(key_columns, key_sizes, aggregation_state_cache);
        mergeStreamsImplCase(
            aggregates_pool, state, data, no_more_keys, overflow_row, row_begin, row_end, aggregate_columns_data, is_cancelled, arena_for_keys);

        consecutive_keys_cache_stats.update(row_end - row_begin, state.getCacheMissesSinceLastReset());
    }
    else
    {
        typename Method::StateNoCache state(key_columns, key_sizes, aggregation_state_cache);
        mergeStreamsImplCase(
            aggregates_pool, state, data, no_more_keys, overflow_row, row_begin, row_end, aggregate_columns_data, is_cancelled, arena_for_keys);
    }
}


void NO_INLINE Aggregator::mergeBlockWithoutKeyStreamsImpl(
    Block block,
    AggregatedDataVariants & result,
    std::atomic<bool> & is_cancelled) const
{
    AggregateColumnsConstData aggregate_columns = params.makeAggregateColumnsData(block);
    mergeWithoutKeyStreamsImpl(result, 0, block.rows(), aggregate_columns, is_cancelled);
}

void NO_INLINE Aggregator::mergeWithoutKeyStreamsImpl(
    AggregatedDataVariants & result,
    size_t row_begin,
    size_t row_end,
    const AggregateColumnsConstData & aggregate_columns_data,
    std::atomic<bool> & is_cancelled) const
{
    using namespace CurrentMetrics;

    AggregatedDataWithoutKey & res = result.without_key;
    if (!res)
    {
        AggregateDataPtr place = result.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        res = place;
    }

    for (size_t row = row_begin; row < row_end; ++row)
    {
        /// Adding Values
        for (size_t i = 0; i < params.aggregates_size; ++i)
        {
            if (aggregate_functions[i]->isParallelizeMergePrepareNeeded())
            {
                std::vector<AggregateDataPtr> data_vec{res + offsets_of_aggregate_states[i], (*aggregate_columns_data[i])[row]};
                aggregate_functions[i]->parallelizeMergePrepare(data_vec, thread_pool, is_cancelled);
            }

            if (aggregate_functions[i]->isAbleToParallelizeMerge())
                aggregate_functions[i]->merge(
                    res + offsets_of_aggregate_states[i], (*aggregate_columns_data[i])[row], thread_pool, is_cancelled, result.aggregates_pool);
            else
                aggregate_functions[i]->merge(
                    res + offsets_of_aggregate_states[i], (*aggregate_columns_data[i])[row], result.aggregates_pool);
        }
    }
}


bool Aggregator::mergeOnBlock(Block block, AggregatedDataVariants & result, bool & no_more_keys, std::atomic<bool> & is_cancelled) const
{
    /// `result` will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    /// How to perform the aggregation?
    if (result.empty())
    {
        result.init(method_chosen);
        result.keys_size = params.keys_size;
        result.key_sizes = key_sizes;
        LOG_TRACE(log, "Aggregation method: {}", result.getMethodName());
    }

    if ((params.overflow_row || result.type == AggregatedDataVariants::Type::without_key) && !result.without_key)
    {
        AggregateDataPtr place = result.aggregates_pool->alignedAlloc(total_size_of_aggregate_states, align_aggregate_states);
        createAggregateStates(place);
        result.without_key = place;
    }

    if (result.type == AggregatedDataVariants::Type::without_key || block.info.is_overflows)
        mergeBlockWithoutKeyStreamsImpl(std::move(block), result, is_cancelled);
#define M(NAME, IS_TWO_LEVEL) \
    else if (result.type == AggregatedDataVariants::Type::NAME) mergeStreamsImpl( \
        std::move(block), \
        result.aggregates_pool, \
        *result.NAME, \
        result.NAME->data, \
        result.without_key, \
        result.consecutive_keys_cache_stats, \
        no_more_keys, \
        is_cancelled);

    APPLY_FOR_AGGREGATED_VARIANTS(M)
#undef M
    else if (result.type != AggregatedDataVariants::Type::without_key)
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");

    size_t result_size = result.sizeWithoutOverflowRow();
    Int64 current_memory_usage = getCurrentQueryMemoryUsage();

    /// Here all the results in the sum are taken into account, from different threads.
    auto result_size_bytes = current_memory_usage - memory_usage_before_aggregation;

    bool worth_convert_to_two_level = worthConvertToTwoLevel(
        params.group_by_two_level_threshold, result_size, params.group_by_two_level_threshold_bytes, result_size_bytes);

    /** Converting to a two-level data structure.
      * It allows you to make, in the subsequent, an effective merge - either economical from memory or parallel.
      */
    if (result.isConvertibleToTwoLevel() && worth_convert_to_two_level)
        result.convertToTwoLevel();

    /// Checking the constraints.
    if (!checkLimits(result_size, no_more_keys))
        return false;

    /** Flush data to disk if too much RAM is consumed.
      * Data can only be flushed to disk if a two-level aggregation structure is used.
      */
    if (params.max_bytes_before_external_group_by
        && result.isTwoLevel()
        && current_memory_usage > static_cast<Int64>(params.max_bytes_before_external_group_by)
        && worth_convert_to_two_level)
    {
        size_t size = current_memory_usage + params.min_free_disk_space;
        writeToTemporaryFile(result, size);
    }

    return true;
}


void Aggregator::mergeBlocks(BucketToBlocks bucket_to_blocks, AggregatedDataVariants & result, std::atomic<bool> & is_cancelled)
{
    if (bucket_to_blocks.empty())
        return;

    UInt64 total_input_rows = 0;
    for (auto & bucket : bucket_to_blocks)
        for (auto & block : bucket.second)
            total_input_rows += block.rows();

    /** `minus one` means the absence of information about the bucket
      * - in the case of single-level aggregation, as well as for blocks with "overflowing" values.
      * If there is at least one block with a bucket number greater or equal than zero, then there was a two-level aggregation.
      */
    auto max_bucket = bucket_to_blocks.rbegin()->first;
    bool has_two_level = max_bucket >= 0;

    if (has_two_level)
    {
    #define M(NAME) \
        if (method_chosen == AggregatedDataVariants::Type::NAME) \
            method_chosen = AggregatedDataVariants::Type::NAME ## _two_level;

        APPLY_FOR_VARIANTS_CONVERTIBLE_TO_TWO_LEVEL(M)

    #undef M
    }

    /// result will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    result.init(method_chosen);
    result.keys_size = params.keys_size;
    result.key_sizes = key_sizes;

    bool has_blocks_with_unknown_bucket = bucket_to_blocks.contains(-1);

    /// First, parallel the merge for the individual buckets. Then we continue merge the data not allocated to the buckets.
    if (has_two_level)
    {
        /** In this case, no_more_keys is not supported due to the fact that
          *  from different threads it is difficult to update the general state for "other" keys (overflows).
          * That is, the keys in the end can be significantly larger than max_rows_to_group_by.
          */

        LOG_TRACE(log, "Merging partially aggregated two-level data.");

        std::atomic<UInt32> next_bucket_to_merge = 0;

        auto merge_bucket = [&bucket_to_blocks, &result, &is_cancelled, &next_bucket_to_merge, max_bucket, this](Arena * aggregates_pool)
        {
            while (true)
            {
                const Int32 bucket = next_bucket_to_merge.fetch_add(1);

                if (bucket > max_bucket)
                    break;

                if (!bucket_to_blocks.contains(bucket))
                    continue;

                if (is_cancelled.load())
                    return;

                for (Block & block : bucket_to_blocks[bucket])
                {
                    /// Copy to avoid race.
                    auto consecutive_keys_cache_stats_copy = result.consecutive_keys_cache_stats;
                #define M(NAME) \
                    else if (result.type == AggregatedDataVariants::Type::NAME) \
                        mergeStreamsImpl(std::move(block), aggregates_pool, *result.NAME, result.NAME->data.impls[bucket], nullptr, consecutive_keys_cache_stats_copy, false, is_cancelled);

                    if (false) {} // NOLINT
                        APPLY_FOR_VARIANTS_TWO_LEVEL(M)
                #undef M
                    else
                        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");
                }
            }
        };

        /// TODO Make a custom threshold.
        const bool use_thread_pool = params.max_threads > 1 && total_input_rows > 100000;

        if (use_thread_pool)
        {
            ThreadPoolCallbackRunnerLocal<void> runner(thread_pool, "AggregatorPool");
            try
            {
                for (size_t i = 0; i < params.max_threads; ++i)
                {
                    result.aggregates_pools.push_back(std::make_shared<Arena>());
                    Arena * aggregates_pool = result.aggregates_pools.back().get();
                    runner([&merge_bucket, aggregates_pool]() { merge_bucket(aggregates_pool); });
                }
            }
            catch (...)
            {
                is_cancelled.store(true);
                throw;
            }
            runner.waitForAllToFinishAndRethrowFirstError();
        }
        else
        {
            result.aggregates_pools.push_back(std::make_shared<Arena>());
            Arena * aggregates_pool = result.aggregates_pools.back().get();
            merge_bucket(aggregates_pool);
        }

        LOG_TRACE(log, "Merged partially aggregated two-level data.");
    }

    if (has_blocks_with_unknown_bucket)
    {
        LOG_TRACE(log, "Merging partially aggregated single-level data.");

        bool no_more_keys = false;

        BlocksList & blocks = bucket_to_blocks[-1];
        for (Block & block : blocks)
        {
            if (!checkLimits(result.sizeWithoutOverflowRow(), no_more_keys))
                break;

            if (result.type == AggregatedDataVariants::Type::without_key || block.info.is_overflows)
                mergeBlockWithoutKeyStreamsImpl(std::move(block), result, is_cancelled);

        #define M(NAME, IS_TWO_LEVEL) \
            else if (result.type == AggregatedDataVariants::Type::NAME) \
                mergeStreamsImpl(std::move(block), result.aggregates_pool, *result.NAME, result.NAME->data, result.without_key, result.consecutive_keys_cache_stats, no_more_keys, is_cancelled);

            APPLY_FOR_AGGREGATED_VARIANTS(M)
        #undef M
            else if (result.type != AggregatedDataVariants::Type::without_key)
                throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");
        }

        LOG_TRACE(log, "Merged partially aggregated single-level data.");
    }

    CurrentMemoryTracker::check();
}


Block Aggregator::mergeBlocks(BlocksList & blocks, bool final, std::atomic<bool> & is_cancelled)
{
    if (blocks.empty())
        return {};

    auto bucket_num = blocks.front().info.bucket_num;
    bool is_overflows = blocks.front().info.is_overflows;

    LOG_TRACE(log, "Merging partially aggregated blocks (bucket = {}).", bucket_num);
    Stopwatch watch;

    /** If possible, change 'method' to some_hash64. Otherwise, leave as is.
      * Better hash function is needed because during external aggregation,
      *  we may merge partitions of data with total number of keys far greater than 4 billion.
      */
    auto merge_method = method_chosen;

#define APPLY_FOR_VARIANTS_THAT_MAY_USE_BETTER_HASH_FUNCTION(M) \
        M(key64)                          \
        M(key_string)                     \
        M(key_fixed_string)               \
        M(keys128)                        \
        M(keys256)                        \
        M(serialized)                     \
        M(nullable_serialized)            \
        M(prealloc_serialized)            \
        M(nullable_prealloc_serialized)   \

#define M(NAME) \
    if (merge_method == AggregatedDataVariants::Type::NAME) \
        merge_method = AggregatedDataVariants::Type::NAME ## _hash64; \

    APPLY_FOR_VARIANTS_THAT_MAY_USE_BETTER_HASH_FUNCTION(M)
#undef M

#undef APPLY_FOR_VARIANTS_THAT_MAY_USE_BETTER_HASH_FUNCTION

    /// Temporary data for aggregation.
    AggregatedDataVariants result;

    /// result will destroy the states of aggregate functions in the destructor
    result.aggregator = this;

    result.init(merge_method);
    result.keys_size = params.keys_size;
    result.key_sizes = key_sizes;

    size_t source_rows = 0;

    /// In some aggregation methods (e.g. serialized) aggregates pools are used also to store serialized aggregation keys.
    /// Memory occupied by them will have the same lifetime as aggregate function states, while it is not actually necessary and leads to excessive memory consumption.
    /// To avoid this we use a separate arena to allocate memory for aggregation keys. Its memory will be freed at this function return.
    auto arena_for_keys = std::make_shared<Arena>();

    for (Block & block : blocks)
    {
        source_rows += block.rows();

        if (bucket_num >= 0 && block.info.bucket_num != bucket_num)
            bucket_num = -1;

        if (result.type == AggregatedDataVariants::Type::without_key || is_overflows)
            mergeBlockWithoutKeyStreamsImpl(std::move(block), result, is_cancelled);

#define M(NAME, IS_TWO_LEVEL) \
    else if (result.type == AggregatedDataVariants::Type::NAME) \
        mergeStreamsImpl(std::move(block), result.aggregates_pool, *result.NAME, result.NAME->data, nullptr, result.consecutive_keys_cache_stats, false, is_cancelled, arena_for_keys.get());

        APPLY_FOR_AGGREGATED_VARIANTS(M)
    #undef M
        else if (result.type != AggregatedDataVariants::Type::without_key)
            throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");
    }

    Block block;
    if (result.type == AggregatedDataVariants::Type::without_key || is_overflows)
    {
        block = prepareBlockAndFillWithoutKey(result, final, is_overflows);
    }
    else
    {
        // Used during memory efficient merging (SortingAggregatedTransform expects single chunk for each bucket_id).
        constexpr bool return_single_block = true;
        block = prepareBlockAndFillSingleLevel<return_single_block>(result, final);
    }
    /// NOTE: two-level data is not possible here - chooseAggregationMethod chooses only among single-level methods.

    if (!final)
    {
        /// Pass ownership of aggregate function states from result to ColumnAggregateFunction objects in the resulting block.
        result.aggregator = nullptr;
    }

    size_t rows = block.rows();
    size_t bytes = block.bytes();
    double elapsed_seconds = watch.elapsedSeconds();
    LOG_DEBUG(
        log,
        "Merged partially aggregated blocks for bucket #{}. Got {} rows, {} from {} source rows in {} sec. ({:.3f} rows/sec., {}/sec.)",
        bucket_num,
        rows,
        ReadableSize(bytes),
        source_rows,
        elapsed_seconds,
        rows / elapsed_seconds,
        ReadableSize(bytes / elapsed_seconds));

    block.info.bucket_num = bucket_num;
    return block;
}

template <typename Method>
void NO_INLINE Aggregator::convertBlockToTwoLevelImpl(
    Method & method,
    Arena * pool,
    ColumnRawPtrs & key_columns,
    const Block & source,
    std::vector<Block> & destinations) const
{
    typename Method::State state(key_columns, key_sizes, aggregation_state_cache);

    size_t rows = source.rows();
    size_t columns = source.columns();

    /// Create a 'selector' that will contain bucket index for every row. It will be used to scatter rows to buckets.
    IColumn::Selector selector(rows);

    /// For every row.
    for (size_t i = 0; i < rows; ++i)
    {
        if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
        {
            if (state.isNullAt(i))
            {
                selector[i] = 0;
                continue;
            }
        }

        /// Calculate bucket number from row hash.
        auto hash = state.getHash(method.data, i, *pool);
        auto bucket = method.data.getBucketFromHash(hash);

        selector[i] = bucket;
    }

    UInt32 num_buckets = static_cast<UInt32>(destinations.size());

    for (size_t column_idx = 0; column_idx < columns; ++column_idx)
    {
        const ColumnWithTypeAndName & src_col = source.getByPosition(column_idx);
        MutableColumns scattered_columns = src_col.column->scatter(num_buckets, selector);

        for (UInt32 bucket = 0, size = num_buckets; bucket < size; ++bucket)
        {
            if (!scattered_columns[bucket]->empty())
            {
                Block & dst = destinations[bucket];
                dst.info.bucket_num = static_cast<int>(bucket);
                dst.insert({std::move(scattered_columns[bucket]), src_col.type, src_col.name});
            }

            /** Inserted columns of type ColumnAggregateFunction will own states of aggregate functions
              *  by holding shared_ptr to source column. See ColumnAggregateFunction.h
              */
        }
    }
}


std::vector<Block> Aggregator::convertBlockToTwoLevel(const Block & block) const
{
    if (!block)
        return {};

    AggregatedDataVariants data;

    ColumnRawPtrs key_columns = params.makeRawKeyColumns(block);

    AggregatedDataVariants::Type type = method_chosen;
    data.keys_size = params.keys_size;
    data.key_sizes = key_sizes;

#define M(NAME) \
    else if (type == AggregatedDataVariants::Type::NAME) \
        type = AggregatedDataVariants::Type::NAME ## _two_level;

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_CONVERTIBLE_TO_TWO_LEVEL(M)
#undef M
    else
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");

    data.init(type);

    size_t num_buckets = 0;

#define M(NAME) \
    else if (data.type == AggregatedDataVariants::Type::NAME) \
        num_buckets = data.NAME->data.NUM_BUCKETS;

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_TWO_LEVEL(M)
#undef M
    else
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");

    std::vector<Block> split_blocks(num_buckets);

#define M(NAME) \
    else if (data.type == AggregatedDataVariants::Type::NAME) \
        convertBlockToTwoLevelImpl(*data.NAME, data.aggregates_pool, \
            key_columns, block, split_blocks);

    if (false) {} // NOLINT
    APPLY_FOR_VARIANTS_TWO_LEVEL(M)
#undef M
    else
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");

    return split_blocks;
}


template <typename Method, typename Table>
void NO_INLINE Aggregator::destroyImpl(Table & table) const
{
    table.forEachMapped([&](AggregateDataPtr & data)
    {
        /** If an exception (usually a lack of memory, the MemoryTracker throws) arose
          *  after inserting the key into a hash table, but before creating all states of aggregate functions,
          *  then data will be equal nullptr.
          */
        if (nullptr == data)
            return;

        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(data + offsets_of_aggregate_states[i]);

        data = nullptr;
    });

    if constexpr (Method::low_cardinality_optimization || Method::one_key_nullable_optimization)
    {
        if (table.getNullKeyData() != nullptr)
        {
            for (size_t i = 0; i < params.aggregates_size; ++i)
                aggregate_functions[i]->destroy(table.getNullKeyData() + offsets_of_aggregate_states[i]);

            table.getNullKeyData() = nullptr;
        }
    }
}


void Aggregator::destroyWithoutKey(AggregatedDataVariants & result) const
{
    AggregatedDataWithoutKey & res_data = result.without_key;

    if (nullptr != res_data)
    {
        for (size_t i = 0; i < params.aggregates_size; ++i)
            aggregate_functions[i]->destroy(res_data + offsets_of_aggregate_states[i]);

        res_data = nullptr;
    }
}


void Aggregator::destroyAllAggregateStates(AggregatedDataVariants & result) const
{
    if (result.empty())
        return;

    /// In what data structure is the data aggregated?
    if (result.type == AggregatedDataVariants::Type::without_key || params.overflow_row)
        destroyWithoutKey(result);

#define M(NAME, IS_TWO_LEVEL) \
    else if (result.type == AggregatedDataVariants::Type::NAME) \
        destroyImpl<decltype(result.NAME)::element_type>(result.NAME->data);

    if (false) {} // NOLINT
    APPLY_FOR_AGGREGATED_VARIANTS(M)
#undef M
    else if (result.type != AggregatedDataVariants::Type::without_key)
        throw Exception(ErrorCodes::UNKNOWN_AGGREGATED_DATA_VARIANT, "Unknown aggregated data variant.");
}

UInt64 calculateCacheKey(const DB::ASTPtr & select_query)
{
    if (!select_query)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Query ptr cannot be null");

    const auto & select = select_query->as<DB::ASTSelectQuery &>();

    // It may happen in some corner cases like `select 1 as num group by num`.
    if (!select.tables())
        return 0;

    SipHash hash;
    hash.update(select.tables()->getTreeHash(/*ignore_aliases=*/true));
    if (const auto prewhere = select.prewhere())
        hash.update(prewhere->getTreeHash(/*ignore_aliases=*/true));
    if (const auto where = select.where())
        hash.update(where->getTreeHash(/*ignore_aliases=*/true));
    if (const auto group_by = select.groupBy())
        hash.update(group_by->getTreeHash(/*ignore_aliases=*/true));
    return hash.get64();
}
}
