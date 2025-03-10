// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Author: kenton@google.com (Kenton Varda)
//  Based on original Protocol Buffers design by
//  Sanjay Ghemawat, Jeff Dean, and others.

#include "google/protobuf/compiler/java/field.h"

#include <memory>
#include <string>

#include "google/protobuf/stubs/logging.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/substitute.h"
#include "google/protobuf/compiler/java/context.h"
#include "google/protobuf/compiler/java/enum_field.h"
#include "google/protobuf/compiler/java/enum_field_lite.h"
#include "google/protobuf/compiler/java/helpers.h"
#include "google/protobuf/compiler/java/map_field.h"
#include "google/protobuf/compiler/java/map_field_lite.h"
#include "google/protobuf/compiler/java/message_field.h"
#include "google/protobuf/compiler/java/message_field_lite.h"
#include "google/protobuf/compiler/java/primitive_field.h"
#include "google/protobuf/compiler/java/primitive_field_lite.h"
#include "google/protobuf/compiler/java/string_field.h"
#include "google/protobuf/compiler/java/string_field_lite.h"
#include "google/protobuf/io/printer.h"


namespace google {
namespace protobuf {
namespace compiler {
namespace java {

namespace {

ImmutableFieldGenerator* MakeImmutableGenerator(const FieldDescriptor* field,
                                                int messageBitIndex,
                                                int builderBitIndex,
                                                Context* context) {
  if (field->is_repeated()) {
    switch (GetJavaType(field)) {
      case JAVATYPE_MESSAGE:
        if (IsMapEntry(field->message_type())) {
          return new ImmutableMapFieldGenerator(field, messageBitIndex,
                                                builderBitIndex, context);
        } else {
          return new RepeatedImmutableMessageFieldGenerator(
              field, messageBitIndex, builderBitIndex, context);
        }
      case JAVATYPE_ENUM:
        return new RepeatedImmutableEnumFieldGenerator(
            field, messageBitIndex, builderBitIndex, context);
      case JAVATYPE_STRING:
        return new RepeatedImmutableStringFieldGenerator(
            field, messageBitIndex, builderBitIndex, context);
      default:
        return new RepeatedImmutablePrimitiveFieldGenerator(
            field, messageBitIndex, builderBitIndex, context);
    }
  } else {
    if (IsRealOneof(field)) {
      switch (GetJavaType(field)) {
        case JAVATYPE_MESSAGE:
          return new ImmutableMessageOneofFieldGenerator(
              field, messageBitIndex, builderBitIndex, context);
        case JAVATYPE_ENUM:
          return new ImmutableEnumOneofFieldGenerator(field, messageBitIndex,
                                                      builderBitIndex, context);
        case JAVATYPE_STRING:
          return new ImmutableStringOneofFieldGenerator(
              field, messageBitIndex, builderBitIndex, context);
        default:
          return new ImmutablePrimitiveOneofFieldGenerator(
              field, messageBitIndex, builderBitIndex, context);
      }
    } else {
      switch (GetJavaType(field)) {
        case JAVATYPE_MESSAGE:
          return new ImmutableMessageFieldGenerator(field, messageBitIndex,
                                                    builderBitIndex, context);
        case JAVATYPE_ENUM:
          return new ImmutableEnumFieldGenerator(field, messageBitIndex,
                                                 builderBitIndex, context);
        case JAVATYPE_STRING:
          return new ImmutableStringFieldGenerator(field, messageBitIndex,
                                                   builderBitIndex, context);
        default:
          return new ImmutablePrimitiveFieldGenerator(field, messageBitIndex,
                                                      builderBitIndex, context);
      }
    }
  }
}

ImmutableFieldLiteGenerator* MakeImmutableLiteGenerator(
    const FieldDescriptor* field, int messageBitIndex, Context* context) {
  if (field->is_repeated()) {
    switch (GetJavaType(field)) {
      case JAVATYPE_MESSAGE:
        if (IsMapEntry(field->message_type())) {
          return new ImmutableMapFieldLiteGenerator(field, messageBitIndex,
                                                    context);
        } else {
          return new RepeatedImmutableMessageFieldLiteGenerator(
              field, messageBitIndex, context);
        }
      case JAVATYPE_ENUM:
        return new RepeatedImmutableEnumFieldLiteGenerator(
            field, messageBitIndex, context);
      case JAVATYPE_STRING:
        return new RepeatedImmutableStringFieldLiteGenerator(
            field, messageBitIndex, context);
      default:
        return new RepeatedImmutablePrimitiveFieldLiteGenerator(
            field, messageBitIndex, context);
    }
  } else {
    if (IsRealOneof(field)) {
      switch (GetJavaType(field)) {
        case JAVATYPE_MESSAGE:
          return new ImmutableMessageOneofFieldLiteGenerator(
              field, messageBitIndex, context);
        case JAVATYPE_ENUM:
          return new ImmutableEnumOneofFieldLiteGenerator(
              field, messageBitIndex, context);
        case JAVATYPE_STRING:
          return new ImmutableStringOneofFieldLiteGenerator(
              field, messageBitIndex, context);
        default:
          return new ImmutablePrimitiveOneofFieldLiteGenerator(
              field, messageBitIndex, context);
      }
    } else {
      switch (GetJavaType(field)) {
        case JAVATYPE_MESSAGE:
          return new ImmutableMessageFieldLiteGenerator(field, messageBitIndex,
                                                        context);
        case JAVATYPE_ENUM:
          return new ImmutableEnumFieldLiteGenerator(field, messageBitIndex,
                                                     context);
        case JAVATYPE_STRING:
          return new ImmutableStringFieldLiteGenerator(field, messageBitIndex,
                                                       context);
        default:
          return new ImmutablePrimitiveFieldLiteGenerator(
              field, messageBitIndex, context);
      }
    }
  }
}


static inline void ReportUnexpectedPackedFieldsCall(io::Printer* printer) {
  // Reaching here indicates a bug. Cases are:
  //   - This FieldGenerator should support packing,
  //     but this method should be overridden.
  //   - This FieldGenerator doesn't support packing, and this method
  //     should never have been called.
  GOOGLE_LOG(FATAL) << "GenerateBuilderParsingCodeFromPacked() "
             << "called on field generator that does not support packing.";
}

}  // namespace

ImmutableFieldGenerator::~ImmutableFieldGenerator() {}

void ImmutableFieldGenerator::GenerateBuilderParsingCodeFromPacked(
    io::Printer* printer) const {
  ReportUnexpectedPackedFieldsCall(printer);
}

ImmutableFieldLiteGenerator::~ImmutableFieldLiteGenerator() {}

// ===================================================================

template <>
FieldGeneratorMap<ImmutableFieldGenerator>::FieldGeneratorMap(
    const Descriptor* descriptor, Context* context)
    : descriptor_(descriptor), field_generators_(descriptor->field_count()) {
  // Construct all the FieldGenerators and assign them bit indices for their
  // bit fields.
  int messageBitIndex = 0;
  int builderBitIndex = 0;
  for (int i = 0; i < descriptor->field_count(); i++) {
    ImmutableFieldGenerator* generator = MakeImmutableGenerator(
        descriptor->field(i), messageBitIndex, builderBitIndex, context);
    field_generators_[i].reset(generator);
    messageBitIndex += generator->GetNumBitsForMessage();
    builderBitIndex += generator->GetNumBitsForBuilder();
  }
}

template <>
FieldGeneratorMap<ImmutableFieldGenerator>::~FieldGeneratorMap() {}

template <>
FieldGeneratorMap<ImmutableFieldLiteGenerator>::FieldGeneratorMap(
    const Descriptor* descriptor, Context* context)
    : descriptor_(descriptor), field_generators_(descriptor->field_count()) {
  // Construct all the FieldGenerators and assign them bit indices for their
  // bit fields.
  int messageBitIndex = 0;
  for (int i = 0; i < descriptor->field_count(); i++) {
    ImmutableFieldLiteGenerator* generator = MakeImmutableLiteGenerator(
        descriptor->field(i), messageBitIndex, context);
    field_generators_[i].reset(generator);
    messageBitIndex += generator->GetNumBitsForMessage();
  }
}

template <>
FieldGeneratorMap<ImmutableFieldLiteGenerator>::~FieldGeneratorMap() {}


void SetCommonFieldVariables(
    const FieldDescriptor* descriptor, const FieldGeneratorInfo* info,
    absl::flat_hash_map<absl::string_view, std::string>* variables) {
  (*variables)["field_name"] = descriptor->name();
  (*variables)["name"] = info->name;
  (*variables)["classname"] = descriptor->containing_type()->name();
  (*variables)["capitalized_name"] = info->capitalized_name;
  (*variables)["disambiguated_reason"] = info->disambiguated_reason;
  (*variables)["constant_name"] = FieldConstantName(descriptor);
  (*variables)["number"] = absl::StrCat(descriptor->number());
  (*variables)["kt_dsl_builder"] = "_builder";
  // These variables are placeholders to pick out the beginning and ends of
  // identifiers for annotations (when doing so with existing variables would
  // be ambiguous or impossible). They should never be set to anything but the
  // empty string.
  (*variables)["{"] = "";
  (*variables)["}"] = "";
  (*variables)["kt_name"] =
      IsForbiddenKotlin(info->name) ? info->name + "_" : info->name;
  (*variables)["kt_capitalized_name"] = IsForbiddenKotlin(info->name)
                                            ? info->capitalized_name + "_"
                                            : info->capitalized_name;
  if (!descriptor->is_repeated()) {
    (*variables)["annotation_field_type"] = FieldTypeName(descriptor->type());
  } else if (GetJavaType(descriptor) == JAVATYPE_MESSAGE &&
             IsMapEntry(descriptor->message_type())) {
    (*variables)["annotation_field_type"] =
        std::string(FieldTypeName(descriptor->type())) + "MAP";
  } else {
    (*variables)["annotation_field_type"] =
        std::string(FieldTypeName(descriptor->type())) + "_LIST";
    if (descriptor->is_packed()) {
      variables->insert(
          {"annotation_field_type",
           absl::StrCat((*variables)["annotation_field_type"], "_PACKED")});
    }
  }
}

void SetCommonOneofVariables(
    const FieldDescriptor* descriptor, const OneofGeneratorInfo* info,
    absl::flat_hash_map<absl::string_view, std::string>* variables) {
  (*variables)["oneof_name"] = info->name;
  (*variables)["oneof_capitalized_name"] = info->capitalized_name;
  (*variables)["oneof_index"] =
      absl::StrCat(descriptor->containing_oneof()->index());
  (*variables)["oneof_stored_type"] = GetOneofStoredType(descriptor);
  (*variables)["set_oneof_case_message"] =
      info->name + "Case_ = " + absl::StrCat(descriptor->number());
  (*variables)["clear_oneof_case_message"] = info->name + "Case_ = 0";
  (*variables)["has_oneof_case_message"] =
      info->name + "Case_ == " + absl::StrCat(descriptor->number());
}

void PrintExtraFieldInfo(
    const absl::flat_hash_map<absl::string_view, std::string>& variables,
    io::Printer* printer) {
  auto it = variables.find("disambiguated_reason");
  if (it != variables.end() && !it->second.empty()) {
    printer->Print(
        variables,
        "// An alternative name is used for field \"$field_name$\" because:\n"
        "//     $disambiguated_reason$\n");
  }
}

}  // namespace java
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
