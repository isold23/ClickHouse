#pragma once

#include "config.h"

#if USE_AVRO

#include <Processors/Formats/Impl/AvroRowInputFormat.h>

namespace Iceberg
{

DB::MutableColumns parseAvro(avro::DataFileReaderBase & file_reader, const DB::Block & header, const DB::FormatSettings & settings);


std::string getFilePath(std::string_view data_path, const std::string & common_path);
}

#endif
