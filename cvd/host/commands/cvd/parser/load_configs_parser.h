/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>
#include <vector>

#include <json/json.h>

#include "common/libs/utils/result.h"
#include "host/commands/cvd/parser/fetch_cvd_parser.h"

namespace cuttlefish {

typedef struct _CvdFlags {
  std::vector<std::string> launch_cvd_flags;
  std::vector<std::string> selector_flags;
  std::vector<std::string> fetch_cvd_flags;
} CvdFlags;

struct LoadDirectories {
  std::string target_directory;
  std::vector<std::string> target_subdirectories;
  std::string launch_home_directory;
  std::string first_instance_directory;
  std::string system_image_directory_flag;
};

Result<Json::Value> ParseJsonFile(const std::string& file_path);

Result<Json::Value> GetOverridedJsonConfig(
    const std::string& config_path,
    const std::vector<std::string>& override_flags);

Result<LoadDirectories> GenerateLoadDirectories(
    const std::string& parent_directory, const int num_instances);

Result<CvdFlags> ParseCvdConfigs(Json::Value& root,
                                 const LoadDirectories& load_directories);

};  // namespace cuttlefish
