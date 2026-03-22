#pragma once

#include <string>

struct DngWriterRuntimeSummary {
  bool enabled = false;
  bool host_created = false;
  bool negative_created = false;
  std::string status;
};

DngWriterRuntimeSummary BuildDngWriterRuntimeSummary(const std::string& compression);

struct DngWriteResult {
  bool ok = false;
  std::string message;
};

DngWriteResult WriteLinearDngFromRaw(const std::string& source_path,
                                     const std::string& output_path,
                                     const std::string& compression);

DngWriteResult WriteSyntheticLinearDng(const std::string& output_path,
                                       const std::string& compression);