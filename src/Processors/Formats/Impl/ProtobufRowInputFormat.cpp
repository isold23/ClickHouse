#include "ProtobufRowInputFormat.h"

#if USE_PROTOBUF
#   include <Columns/IColumn.h>
#   include <Core/Block.h>
#   include <Formats/FormatFactory.h>
#   include <Formats/ProtobufReader.h>
#   include <Formats/ProtobufSchemas.h>
#   include <Formats/ProtobufSerializer.h>

namespace DB
{

ProtobufRowInputFormat::ProtobufRowInputFormat(
    ReadBuffer & in_,
    const Block & header_,
    const Params & params_,
    const ProtobufSchemaInfo & schema_info_,
    bool with_length_delimiter_,
    bool flatten_google_wrappers_,
    const String & google_protos_path)
    : IRowInputFormat(header_, in_, params_)
    , descriptor(ProtobufSchemas::instance().getMessageTypeForFormatSchema(
          schema_info_.getSchemaInfo(), ProtobufSchemas::WithEnvelope::No, google_protos_path))
    , with_length_delimiter(with_length_delimiter_)
    , flatten_google_wrappers(flatten_google_wrappers_)
{
}

void ProtobufRowInputFormat::createReaderAndSerializer()
{
    reader = std::make_unique<ProtobufReader>(*in);
    serializer = ProtobufSerializer::create(
        getPort().getHeader().getNames(),
        getPort().getHeader().getDataTypes(),
        missing_column_indices,
        descriptor,
        with_length_delimiter,
        /* with_envelope = */ false,
        flatten_google_wrappers,
        *reader);
}

bool ProtobufRowInputFormat::readRow(MutableColumns & columns, RowReadExtension & row_read_extension)
{
    if (!reader)
        createReaderAndSerializer();

    if (reader->eof())
        return false;

    size_t row_num = columns.empty() ? 0 : columns[0]->size();
    if (!row_num)
        serializer->setColumns(columns.data(), columns.size());

    serializer->readRow(row_num);

    row_read_extension.read_columns.clear();
    row_read_extension.read_columns.resize(columns.size(), true);
    for (size_t column_idx : missing_column_indices)
        row_read_extension.read_columns[column_idx] = false;
    return true;
}

void ProtobufRowInputFormat::setReadBuffer(ReadBuffer & in_)
{
    if (reader)
        reader->setReadBuffer(in_);
    IRowInputFormat::setReadBuffer(in_);
}

bool ProtobufRowInputFormat::allowSyncAfterError() const
{
    return true;
}

void ProtobufRowInputFormat::syncAfterError()
{
    reader->endMessage(true);
}

void ProtobufRowInputFormat::resetParser()
{
    IRowInputFormat::resetParser();
    serializer.reset();
    reader.reset();
}

size_t ProtobufRowInputFormat::countRows(size_t max_block_size)
{
    if (!reader)
        createReaderAndSerializer();

    size_t num_rows = 0;
    while (!reader->eof() && num_rows < max_block_size)
    {
        reader->startMessage(with_length_delimiter);
        reader->endMessage(false);
        ++num_rows;
    }

    return num_rows;
}

void registerInputFormatProtobuf(FormatFactory & factory)
{
    for (bool with_length_delimiter : {false, true})
    {
        factory.registerInputFormat(
            with_length_delimiter ? "Protobuf" : "ProtobufSingle",
            [with_length_delimiter](ReadBuffer & buf, const Block & sample, IRowInputFormat::Params params, const FormatSettings & settings)
            {
                return std::make_shared<ProtobufRowInputFormat>(
                    buf,
                    sample,
                    std::move(params),
                    ProtobufSchemaInfo(settings, "Protobuf", sample, settings.protobuf.use_autogenerated_schema),
                    with_length_delimiter,
                    settings.protobuf.input_flatten_google_wrappers,
                    settings.protobuf.google_protos_path);
            });
        factory.markFormatSupportsSubsetOfColumns(with_length_delimiter ? "Protobuf" : "ProtobufSingle");
    }
}

ProtobufSchemaReader::ProtobufSchemaReader(const FormatSettings & format_settings)
    : schema_info(
        format_settings.schema.format_schema, "Protobuf", true, format_settings.schema.is_server, format_settings.schema.format_schema_path)
    , skip_unsupported_fields(format_settings.protobuf.skip_fields_with_unsupported_types_in_schema_inference)
    , google_protos_path(format_settings.protobuf.google_protos_path)
{
}

NamesAndTypesList ProtobufSchemaReader::readSchema()
{
    auto descriptor = ProtobufSchemas::instance().getMessageTypeForFormatSchema(
        schema_info, ProtobufSchemas::WithEnvelope::No, google_protos_path);
    return protobufSchemaToCHSchema(descriptor.message_descriptor, skip_unsupported_fields);
}

void registerProtobufSchemaReader(FormatFactory & factory)
{
    factory.registerExternalSchemaReader("Protobuf", [](const FormatSettings & settings)
    {
        return std::make_shared<ProtobufSchemaReader>(settings);
    });
    factory.registerFileExtension("pb", "Protobuf");

    factory.registerExternalSchemaReader("ProtobufSingle", [](const FormatSettings & settings)
    {
        return std::make_shared<ProtobufSchemaReader>(settings);
    });

    for (const auto & name : {"Protobuf", "ProtobufSingle"})
        factory.registerAdditionalInfoForSchemaCacheGetter(
            name,
            [](const FormatSettings & settings)
            {
                return fmt::format(
                    "format_schema={}, skip_fields_with_unsupported_types_in_schema_inference={}",
                    settings.schema.format_schema,
                    settings.protobuf.skip_fields_with_unsupported_types_in_schema_inference);
            });
}

}

#else

namespace DB
{
class FormatFactory;
void registerInputFormatProtobuf(FormatFactory &) {}
void registerProtobufSchemaReader(FormatFactory &) {}
}

#endif
