#include <Core/Defines.h>
#include <Core/ProtocolDefines.h>

#include <IO/ReadHelpers.h>
#include <IO/VarInt.h>
#include <Compression/CompressedReadBufferFromFile.h>

#include <DataTypes/DataTypeFactory.h>
#include <Columns/ColumnLazy.h>
#include <Columns/ColumnTuple.h>
#include <Columns/IColumnLazyHelper.h>
#include <Common/typeid_cast.h>
#include <base/range.h>

#include <Formats/NativeReader.h>
#include <Formats/insertNullAsDefaultIfNeeded.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/Serializations/SerializationInfo.h>
#include <DataTypes/DataTypeAggregateFunction.h>

#include <Interpreters/castColumn.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_INDEX;
    extern const int LOGICAL_ERROR;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int INCORRECT_DATA;
    extern const int TOO_LARGE_ARRAY_SIZE;
}


NativeReader::NativeReader(ReadBuffer & istr_, UInt64 server_revision_)
    : istr(istr_), server_revision(server_revision_)
{
}

NativeReader::NativeReader(
    ReadBuffer & istr_,
    const Block & header_,
    UInt64 server_revision_,
    bool skip_unknown_columns_,
    bool null_as_default_,
    bool allow_types_conversion_,
    BlockMissingValues * block_missing_values_)
    : istr(istr_)
    , header(header_)
    , server_revision(server_revision_)
    , skip_unknown_columns(skip_unknown_columns_)
    , null_as_default(null_as_default_)
    , allow_types_conversion(allow_types_conversion_)
    , block_missing_values(block_missing_values_)
{
}

NativeReader::NativeReader(ReadBuffer & istr_, UInt64 server_revision_,
    IndexForNativeFormat::Blocks::const_iterator index_block_it_,
    IndexForNativeFormat::Blocks::const_iterator index_block_end_)
    : istr(istr_), server_revision(server_revision_),
    use_index(true), index_block_it(index_block_it_), index_block_end(index_block_end_)
{
    istr_concrete = typeid_cast<CompressedReadBufferFromFile *>(&istr);
    if (!istr_concrete)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "When need to use index for NativeReader, istr must be CompressedReadBufferFromFile.");

    if (index_block_it == index_block_end)
        return;

    index_column_it = index_block_it->columns.begin();

    /// Initialize header from the index.
    for (const auto & column : index_block_it->columns)
    {
        auto type = DataTypeFactory::instance().get(column.type);
        header.insert(ColumnWithTypeAndName{ type, column.name });
    }
}

void NativeReader::resetParser()
{
    istr_concrete = nullptr;
    use_index = false;
}

void NativeReader::readData(const ISerialization & serialization, ColumnPtr & column, ReadBuffer & istr, size_t rows, double avg_value_size_hint)
{
    ISerialization::DeserializeBinaryBulkSettings settings;
    settings.getter = [&](ISerialization::SubstreamPath) -> ReadBuffer * { return &istr; };
    settings.avg_value_size_hint = avg_value_size_hint;
    settings.position_independent_encoding = false;
    settings.native_format = true;

    ISerialization::DeserializeBinaryBulkStatePtr state;

    serialization.deserializeBinaryBulkStatePrefix(settings, state);
    serialization.deserializeBinaryBulkWithMultipleStreams(column, rows, settings, state, nullptr);

    if (column->size() != rows)
        throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA,
            "Cannot read all data in NativeReader. Rows read: {}. Rows expected: {}", column->size(), rows);
}


Block NativeReader::getHeader() const
{
    return header;
}


Block NativeReader::read()
{
    Block res;

    const DataTypeFactory & data_type_factory = DataTypeFactory::instance();

    if (use_index && index_block_it == index_block_end)
        return res;

    if (istr.eof())
    {
        if (use_index)
            throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA, "Input doesn't contain all data for index.");

        return res;
    }

    /// Additional information about the block.
    if (server_revision > 0)
        res.info.read(istr);

    /// Dimensions
    size_t columns = 0;
    size_t rows = 0;

    if (!use_index)
    {
        readVarUInt(columns, istr);
        readVarUInt(rows, istr);

        if (columns > 1'000'000uz)
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Suspiciously many columns in Native format: {}", columns);
        if (rows > 1'000'000'000'000uz)
            throw Exception(ErrorCodes::TOO_LARGE_ARRAY_SIZE, "Suspiciously many rows in Native format: {}", rows);
    }
    else
    {
        columns = index_block_it->num_columns;
        rows = index_block_it->num_rows;
    }

    if (columns == 0 && !header && rows != 0)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Zero columns but {} rows in Native format.", rows);

    for (size_t i = 0; i < columns; ++i)
    {
        if (use_index)
        {
            /// If the current position is what is required, the real seek does not occur.
            istr_concrete->seek(index_column_it->location.offset_in_compressed_file, index_column_it->location.offset_in_decompressed_block);
        }

        ColumnWithTypeAndName column;

        /// Name
        readBinary(column.name, istr);

        /// Type
        String type_name;
        readBinary(type_name, istr);
        column.type = data_type_factory.get(type_name);

        setVersionToAggregateFunctions(column.type, true, server_revision);

        SerializationPtr serialization;
        ColumnPtr read_column;
        bool skip_reading = false;
        bool is_lazy_column = false;

        if (const auto * tmp_header_column = header.findByName(column.name))
        {
            if (const auto * column_lazy = checkAndGetColumn<ColumnLazy>(tmp_header_column->column.get()))
            {
                if (column_lazy->getColumnLazyHelper())
                {
                    serialization = column_lazy->getColumnLazyHelper()->getSerialization();
                    const auto & tmp_columns = column_lazy->getColumns();
                    read_column = ColumnTuple::create(tmp_columns)->cloneEmpty();
                }
                else
                {
                    read_column = ColumnLazy::create(rows);
                    skip_reading = true;
                }
                is_lazy_column = true;
            }
        }

        if (is_lazy_column)
        {
        }
        else if (is_lazy_column && server_revision >= DBMS_MIN_REVISION_WITH_CUSTOM_SERIALIZATION)
        {
            auto info = column.type->createSerializationInfo({});

            UInt8 has_custom;
            readBinary(has_custom, istr);
            if (has_custom)
                info->deserializeFromKindsBinary(istr);

            serialization = column.type->getSerialization(*info);
            read_column = column.type->createColumn(*serialization);
        }
        else
        {
            serialization = column.type->getDefaultSerialization();
            read_column = column.type->createColumn(*serialization);
        }

        if (use_index)
        {
            /// Index allows to do more checks.
            if (index_column_it->name != column.name)
                throw Exception(ErrorCodes::INCORRECT_INDEX, "Index points to column with wrong name: corrupted index or data");
            if (index_column_it->type != type_name)
                throw Exception(ErrorCodes::INCORRECT_INDEX, "Index points to column with wrong type: corrupted index or data");
        }

        double avg_value_size_hint = avg_value_size_hints.empty() ? 0 : avg_value_size_hints[i];
        if (!skip_reading && rows)    /// If no rows, nothing to read.
            readData(*serialization, read_column, istr, rows, avg_value_size_hint);

        column.column = std::move(read_column);

        bool use_in_result = true;
        if (header)
        {
            if (header.has(column.name))
            {
                auto & header_column = header.getByName(column.name);

                if (null_as_default)
                    insertNullAsDefaultIfNeeded(column, header_column, header.getPositionByName(column.name), block_missing_values);

                if (!skip_reading)
                {
                    if (const auto * column_lazy = checkAndGetColumn<ColumnLazy>(header_column.column.get()))
                    {
                        if (const auto * column_tuple = typeid_cast<const ColumnTuple *>(column.column.get()))
                            column.column = ColumnLazy::create(column_tuple->getColumns(), column_lazy->getColumnLazyHelper());
                        else
                            throw Exception(ErrorCodes::INCORRECT_DATA, "Unknown column with name {} and data type {} found while reading data in Native format",
                                            column.name,
                                            column.column->getDataType());
                    }
                }

                if (!header_column.type->equals(*column.type))
                {
                    if (allow_types_conversion)
                    {
                        try
                        {
                            column.column = castColumn(column, header_column.type);
                        }
                        catch (Exception & e)
                        {
                            e.addMessage(fmt::format(
                                "while converting column \"{}\" from type {} to type {}",
                                column.name,
                                column.type->getName(),
                                header_column.type->getName()));
                            throw;
                        }
                    }
                    else
                    {
                        /// Support insert from old clients without low cardinality type.
                        column.column = recursiveLowCardinalityTypeConversion(column.column, column.type, header_column.type);
                    }

                    column.type = header_column.type;
                }
            }
            else
            {
                if (!skip_unknown_columns)
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Unknown column with name {} found while reading data in Native format", column.name);
                use_in_result = false;
            }
        }

        if (use_in_result)
            res.insert(std::move(column));

        if (use_index)
            ++index_column_it;
    }

    if (use_index)
    {
        if (index_column_it != index_block_it->columns.end())
            throw Exception(ErrorCodes::INCORRECT_INDEX, "Inconsistent index: not all columns were read");

        ++index_block_it;
        if (index_block_it != index_block_end)
            index_column_it = index_block_it->columns.begin();
    }

    if (rows && header)
    {
        /// Allow to skip columns. Fill them with default values.
        Block tmp_res;

        for (size_t column_i = 0; column_i != header.columns(); ++column_i)
        {
            auto & col = header.getByPosition(column_i);
            if (res.has(col.name))
            {
                tmp_res.insert(res.getByName(col.name));
            }
            else
            {
                tmp_res.insert({col.type->createColumn()->cloneResized(rows), col.type, col.name});
                if (block_missing_values)
                    block_missing_values->setBits(column_i, rows);
            }
        }
        tmp_res.info = res.info;

        res.swap(tmp_res);
    }

    if (res.rows() != rows)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Row count mismatch after desirialization, got: {}, expected: {}", res.rows(), rows);

    return res;
}

void NativeReader::updateAvgValueSizeHints(const Block & block)
{
    auto rows = block.rows();
    if (rows < 10)
        return;

    avg_value_size_hints.resize_fill(block.columns(), 0);

    for (auto idx : collections::range(0, block.columns()))
    {
        auto & avg_value_size_hint = avg_value_size_hints[idx];
        IDataType::updateAvgValueSizeHint(*block.getByPosition(idx).column, avg_value_size_hint);
    }
}

}
