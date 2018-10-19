// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "stratum/lib/utils.h"

#include <stdio.h>
#include <fstream>  // IWYU pragma: keep
#include <string>

#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/util/message_differencer.h"
#include "absl/strings/substitute.h"
#include "stratum/lib/macros.h"
#include "stratum/public/lib/error.h"

namespace stratum {

::util::Status WriteProtoToBinFile(const ::google::google::protobuf::Message& message,
                                   const std::string& filename) {
  std::string buffer;
  if (!message.SerializeToString(&buffer)) {
    return MAKE_ERROR(ERR_INVALID_PARAM)
           << "Failed to convert proto to bin string buffer: "
           << message.ShortDebugString();
  }
  RETURN_IF_ERROR(WriteStringToFile(buffer, filename));

  return ::util::OkStatus();
}

::util::Status ReadProtoFromBinFile(const std::string& filename,
                                    ::google::google::protobuf::Message* message) {
  std::string buffer;
  RETURN_IF_ERROR(ReadFileToString(filename, &buffer));
  if (!message->ParseFromString(buffer)) {
    return MAKE_ERROR(ERR_INTERNAL) << "Failed to parse the binary content of "
                                    << filename << " to proto.";
  }

  return ::util::OkStatus();
}

::util::Status WriteProtoToTextFile(const ::google::google::protobuf::Message& message,
                                    const std::string& filename) {
  std::string text;
  RETURN_IF_ERROR(PrintProtoToString(message, &text));
  RETURN_IF_ERROR(WriteStringToFile(text, filename));

  return ::util::OkStatus();
}

::util::Status ReadProtoFromTextFile(const std::string& filename,
                                     ::google::google::protobuf::Message* message) {
  std::string text;
  RETURN_IF_ERROR(ReadFileToString(filename, &text));
  RETURN_IF_ERROR(ParseProtoFromString(text, message));

  return ::util::OkStatus();
}

::util::Status PrintProtoToString(const ::google::google::protobuf::Message& message,
                                  std::string* text) {
  if (!::google::google::protobuf::TextFormat::PrintToString(message, text)) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Failed to print proto to string: " << message.ShortDebugString();
  }

  return ::util::OkStatus();
}

::util::Status ParseProtoFromString(const std::string& text,
                                    ::google::google::protobuf::Message* message) {
  if (!::google::google::protobuf::TextFormat::ParseFromString(text, message)) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Failed to parse proto from the following string: " << text;
  }

  return ::util::OkStatus();
}

::util::Status WriteStringToFile(const std::string& buffer,
                                 const std::string& filename, bool append) {
  std::ofstream outfile;
  outfile.open(filename.c_str(), append
                                     ? std::ofstream::out | std::ofstream::app
                                     : std::ofstream::out);
  if (!outfile.is_open()) {
    return MAKE_ERROR(ERR_INTERNAL) << "Error when opening " << filename << ".";
  }
  outfile << buffer;
  outfile.close();

  return ::util::OkStatus();
}

::util::Status ReadFileToString(const std::string& filename,
                                std::string* buffer) {
  if (!PathExists(filename)) {
    return MAKE_ERROR(ERR_FILE_NOT_FOUND) << filename << " not found.";
  }
  if (IsDir(filename)) {
    return MAKE_ERROR(ERR_FILE_NOT_FOUND) << filename << " is a dir.";
  }

  std::ifstream infile;
  infile.open(filename.c_str());
  if (!infile.is_open()) {
    return MAKE_ERROR(ERR_INTERNAL) << "Error when opening " << filename << ".";
  }

  std::string contents((std::istreambuf_iterator<char>(infile)),
                       (std::istreambuf_iterator<char>()));
  buffer->append(contents);
  infile.close();

  return ::util::OkStatus();
}

std::string StringToHex(const std::string& str) {
  static const char* const characters = "0123456789ABCDEF";
  std::string hex_str;
  const size_t size = str.size();
  hex_str.reserve(2 * size);
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = str[i];
    hex_str.push_back(characters[c >> 4]);
    hex_str.push_back(characters[c & 0xF]);
  }
  return hex_str;
}

::util::Status RecursivelyCreateDir(const std::string& dir) {
  CHECK_RETURN_IF_FALSE(!dir.empty());

  // Use system() to execute a 'mkdir -p'. This seems to be the simplest, but
  // not necessarily the best solution.
  // TODO: Investigate if there is a better way.
  const std::string& cmd = absl::Substitute("mkdir -p $0", dir.c_str());
  int ret = system(cmd.c_str());
  if (ret != 0) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Failed to execute '" << cmd << "'. Return value: " << ret << ".";
  }

  return ::util::OkStatus();
}

::util::Status RemoveFile(const std::string& path) {
  CHECK_RETURN_IF_FALSE(!path.empty());
  CHECK_RETURN_IF_FALSE(PathExists(path)) << path << " does not exist.";
  CHECK_RETURN_IF_FALSE(!IsDir(path)) << path << " is a dir.";
  int ret = remove(path.c_str());
  if (ret != 0) {
    return MAKE_ERROR(ERR_INTERNAL)
           << "Failed to remove '" << path << "'. Return value: " << ret << ".";
  }

  return ::util::OkStatus();
}

bool PathExists(const std::string& path) {
  struct stat stbuf;
  return (stat(path.c_str(), &stbuf) >= 0);
}

bool IsDir(const std::string& path) {
  struct stat stbuf;
  if (stat(path.c_str(), &stbuf) < 0) {
    return false;
  }
  return S_ISDIR(stbuf.st_mode);
}

std::string DirName(const std::string& path) {
  char* path_str = strdup(path.c_str());
  std::string dir = dirname(path_str);
  free(path_str);
  return dir;
}

std::string BaseName(const std::string& path) {
  char* path_str = strdup(path.c_str());
  std::string base = basename(path_str);
  free(path_str);
  return base;
}

// TODO(aghaffar): At the moment this function will not work well for
// complex messages with repeated fields or maps. Find a better way.
bool ProtoLess(const google::protobuf::Message& m1, const google::protobuf::Message& m2) {
  return m1.SerializeAsString() < m2.SerializeAsString();
}

bool ProtoEqual(const google::protobuf::Message& m1, const google::protobuf::Message& m2) {
  MessageDifferencer differencer;
  differencer.set_repeated_field_comparison(MessageDifferencer::AS_SET);
  return differencer.Compare(m1, m2);
}

// TODO(aghaffar): At the moment this function will not work well for
// complex messages with repeated fields or maps. Find a better way.
size_t ProtoHash(const google::protobuf::Message& m) {
  std::hash<std::string> string_hasher;
  std::string s;
  m.SerializeToString(&s);
  return string_hasher(s);
}

}  // namespace stratum
