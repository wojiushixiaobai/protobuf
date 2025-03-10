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
//
// Utility class for writing text to a ZeroCopyOutputStream.

#ifndef GOOGLE_PROTOBUF_IO_PRINTER_H__
#define GOOGLE_PROTOBUF_IO_PRINTER_H__

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "google/protobuf/stubs/logging.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/types/variant.h"
#include "google/protobuf/io/zero_copy_sink.h"


// Must be included last.
#include "google/protobuf/port_def.inc"

namespace google {
namespace protobuf {
namespace io {
// Records annotations about a Printer's output.
class PROTOBUF_EXPORT AnnotationCollector {
 public:
  // Annotation is a offset range and a payload pair. This payload's layout is
  // specific to derived types of AnnotationCollector.
  using Annotation = std::pair<std::pair<size_t, size_t>, std::string>;

  virtual ~AnnotationCollector() = default;

  // Records that the bytes in file_path beginning with begin_offset and ending
  // before end_offset are associated with the SourceCodeInfo-style path.
  virtual void AddAnnotation(size_t begin_offset, size_t end_offset,
                             const std::string& file_path,
                             const std::vector<int>& path) = 0;

  // TODO(gerbens) I don't see why we need virtuals here. Just a vector of
  // range, payload pairs stored in a context should suffice.
  virtual void AddAnnotationNew(Annotation&) {}
};

// Records annotations about a Printer's output to a Protobuf message,
// assuming that it has a repeated submessage field named `annotation` with
// fields matching
//
// message ??? {
//   repeated int32 path = 1;
//   optional string source_file = 2;
//   optional int32 begin = 3;
//   optional int32 end = 4;
// }
template <typename AnnotationProto>
class AnnotationProtoCollector : public AnnotationCollector {
 public:
  explicit AnnotationProtoCollector(AnnotationProto* annotation_proto)
      : annotation_proto_(annotation_proto) {}

  void AddAnnotation(size_t begin_offset, size_t end_offset,
                     const std::string& file_path,
                     const std::vector<int>& path) override {
    auto* annotation = annotation_proto_->add_annotation();
    for (int i = 0; i < path.size(); ++i) {
      annotation->add_path(path[i]);
    }
    annotation->set_source_file(file_path);
    annotation->set_begin(begin_offset);
    annotation->set_end(end_offset);
  }

  void AddAnnotationNew(Annotation& a) override {
    auto* annotation = annotation_proto_->add_annotation();
    annotation->ParseFromString(a.second);
    annotation->set_begin(a.first.first);
    annotation->set_end(a.first.second);
  }

 private:
  AnnotationProto* annotation_proto_;
};

// A source code printer for assisting in code generation.
//
// This type implements a simple templating language for substiting variables
// into static, user-provided strings, and also tracks indentation
// automatically.
//
// The main entry-point for this type is the Emit function, which can be used
// thus:
//
//   Printer p(output);
//   p.Emit({{"class", my_class_name}}, R"cc(
//     class $class$ {
//      public:
//       $class$(int x) : x_(x) {}
//      private:
//       int x_;
//     };
//   )cc");
//
// Substitutions are of the form $var$, which is looked up in the map passed in
// as the first argument. The variable delimiter character, $, can be chosen to
// be something convenient for the target language. For example, in PHP, which
// makes heavy use of $, it can be made into something like # instead.
//
// A literal $ can be emitted by writing $$.
//
// Substitutions may contain spaces around the name of the variable, which will
// be ignored for the purposes of looking up the variable to substitute in, but
// which will be reproduced in the output:
//
//   p.Emit({{"foo", "bar"}}, "$ foo $");
//
// emits the string " bar ". If the substituted-in variable is the empty string,
// then the surrounding spaces are *not* printed:
//
//   p.Emit({{"xzy", xyz}}, "$xyz $Thing");
//
// If xyz is "Foo", this will become "Foo Thing", but if it is "", this becomes
// "Thing", rather than " Thing". This helps minimize awkward whitespace in the
// output.
//
// The value may be any type that can be stringified with `absl::StrCat`:
//
//   p.Emit({{"num", 5}}, "x = $num$;");
//
// If a variable is referenced in the format string that is missing, the program
// will crash. Callers must statically know that every variable reference is
// valid, and MUST NOT pass user-provided strings directly into Emit().
//
// # Callback Substitution
//
// Instead of passing a string into Emit(), it is possible to pass in a callback
// as a variable mapping. This will take indentation into account, which allows
// factoring out parts of a formatting string while ensuring braces are
// balanced:
//
//   p.Emit(
//     {{"methods", [&] {
//       p.Emit(R"cc(
//         int Bar() {
//            return 42;
//         }
//       )cc");
//     }}},
//     R"cc(
//       class Foo {
//        public:
//         $methods$;
//       };
//     )cc"
//   );
//
// This emits
//
//   class Foo {
//    public:
//     int Bar() {
//       return 42;
//     }
//   };
//
// # Lookup Frames
//
// If many calls to Emit() use the same set of variables, they can be stored
// in a *variable lookup frame*, like so:
//
//   auto vars = p.WithVars({{"class_name", my_class_name}});
//   p.Emit(R"cc(
//     class $class_name$ {
//      public:
//       $class_name$(int x);
//       // Etc.
//     };
//   )cc");
//
// WithVars() returns an RAII object that will "pop" the lookup frame on scope
// exit, ensuring that the variables remain local. There are a few different
// overloads of WithVars(); it accepts a map type, like absl::flat_hash_map,
// either by-value (which will cause the Printer to store a copy), or by
// pointer (which will cause the Printer to store a pointer, potentially
// avoiding a copy.)
//
// p.Emit(vars, "..."); is effectively syntax sugar for
//
//  { auto v = p.WithVars(vars); p.Emit("..."); }
//
// NOTE: callbacks are *not* allowed with WithVars; callbacks should be local
// to a specific Emit() call.
//
// # Annotations
//
// If Printer is given an AnnotationCollector, it will use it to record which
// spans of generated code correspond to user-indicated descriptors. There are
// a few different ways of indicating when to emit annotations.
//
// The WithAnnotations() function is like WithVars(), but accepts maps with
// string keys and descriptor values. It adds an annotation variable frame and
// returns an RAII object that pops the frame.
//
// There are two different ways to annotate code. In the first, when
// substituting a variable, if there is an annotation with the same name, then
// the resulting expanded value's span will be annotated with that annotation.
// For example:
//
//   auto v = p.WithVars({{"class_name", my_class_name}});
//   auto a = p.WithAnnotations({{"class_name", message_descriptor}});
//   p.Emit(R"cc(
//     class $class_name$ {
//      public:
//       $class_name$(int x);
//       // Etc.
//     };
//   )cc");
//
// The span corresponding to whatever $class_name$ expands to will be annotated
// as having come from message_descriptor.
//
// For convenience, this can be done with a single WithVars(), using the special
// three-argument form:
//
//   auto v = p.WithVars({{"class_name", my_class_name, message_descriptor}});
//   p.Emit(R"cc(
//     class $class_name$ {
//      public:
//       $class_name$(int x);
//       // Etc.
//     };
//   )cc");
//
//
// Alternatively, a range may be given explicitly:
//
//   auto a = p.WithAnnotations({{"my_desc", message_descriptor}});
//   p.Emit(R"cc(
//     $_start$my_desc$
//     class Foo {
//       // Etc.
//     };
//     $_end$my_desc$
//   )cc");
//
// The special $_start$ and $_end$ variables indicate the start and end of an
// annotated span, which is annotated with the variable that follows. This
// form can produce somewhat unreadable format strings and is not recommended.
//
// Note that whitespace after a $_start$ and before an $_end$ is not printed.
//
// # Indentation
//
// Printer tracks an indentation amount to add to each new line, independent
// from indentation in an Emit() call's literal. The amount of indentation to
// add is controlled by the WithIndent() function:
//
//   p.Emit("class $class_name$ {");
//   {
//     auto indent = p.WithIndent();
//     p.Emit(R"cc(
//       public:
//        $class_name$(int x);
//     )cc");
//   }
//   p.Emit("};");
//
// This will automatically add one level of indentation to all code in scope of
// `indent`, which is an RAII object much like the return value of `WithVars()`.
//
// # Old API
// TODO(b/242326974): Delete this documentation.
//
// Printer supports an older-style API that is in the process of being
// re-written. The old documentation is reproduced here until all use-cases are
// handled.
//
// This simple utility class assists in code generation.  It basically
// allows the caller to define a set of variables and then output some
// text with variable substitutions.  Example usage:
//
//   Printer printer(output, '$');
//   map<string, string> vars;
//   vars["name"] = "Bob";
//   printer.Print(vars, "My name is $name$.");
//
// The above writes "My name is Bob." to the output stream.
//
// Printer aggressively enforces correct usage, crashing (with assert failures)
// in the case of undefined variables in debug builds. This helps greatly in
// debugging code which uses it.
//
// If a Printer is constructed with an AnnotationCollector, it will provide it
// with annotations that connect the Printer's output to paths that can identify
// various descriptors.  In the above example, if person_ is a descriptor that
// identifies Bob, we can associate the output string "My name is Bob." with
// a source path pointing to that descriptor with:
//
//   printer.Annotate("name", person_);
//
// The AnnotationCollector will be sent an annotation linking the output range
// covering "Bob" to the logical path provided by person_.  Tools may use
// this association to (for example) link "Bob" in the output back to the
// source file that defined the person_ descriptor identifying Bob.
//
// Annotate can only examine variables substituted during the last call to
// Print.  It is invalid to refer to a variable that was used multiple times
// in a single Print call.
//
// In full generality, one may specify a range of output text using a beginning
// substitution variable and an ending variable.  The resulting annotation will
// span from the first character of the substituted value for the beginning
// variable to the last character of the substituted value for the ending
// variable.  For example, the Annotate call above is equivalent to this one:
//
//   printer.Annotate("name", "name", person_);
//
// This is useful if multiple variables combine to form a single span of output
// that should be annotated with the same source path.  For example:
//
//   Printer printer(output, '$');
//   map<string, string> vars;
//   vars["first"] = "Alice";
//   vars["last"] = "Smith";
//   printer.Print(vars, "My name is $first$ $last$.");
//   printer.Annotate("first", "last", person_);
//
// This code would associate the span covering "Alice Smith" in the output with
// the person_ descriptor.
//
// Note that the beginning variable must come before (or overlap with, in the
// case of zero-sized substitution values) the ending variable.
//
// It is also sometimes useful to use variables with zero-sized values as
// markers.  This avoids issues with multiple references to the same variable
// and also allows annotation ranges to span literal text from the Print
// templates:
//
//   Printer printer(output, '$');
//   map<string, string> vars;
//   vars["foo"] = "bar";
//   vars["function"] = "call";
//   vars["mark"] = "";
//   printer.Print(vars, "$function$($foo$,$foo$)$mark$");
//   printer.Annotate("function", "mark", call_);
//
// This code associates the span covering "call(bar,bar)" in the output with the
// call_ descriptor.
class PROTOBUF_EXPORT Printer {
 private:
  // This type exists to work around an absl type that has not yet been
  // released.
  struct SourceLocation {
    static SourceLocation current() { return {}; }
    absl::string_view file_name() { return "<unknown>"; }
    int line() { return 0; }
  };

  struct AnnotationRecord {
    std::vector<int> path;
    std::string file_path;

    // AnnotationRecord's constructors are *not* marked as explicit,
    // specifically so that it is possible to construct a
    // map<string, AnnotationRecord> by writing
    //
    // {{"foo", my_cool_descriptor}, {"bar", "file.proto"}}

    template <
        typename String,
        std::enable_if_t<std::is_convertible<const String&, std::string>::value,
                         int> = 0>
    AnnotationRecord(  // NOLINT(google-explicit-constructor)
        const String& file_path)
        : file_path(file_path) {}

    template <typename Desc,
              // This SFINAE clause excludes char* from matching this
              // constructor.
              std::enable_if_t<std::is_class<Desc>::value, int> = 0>
    AnnotationRecord(const Desc* desc)  // NOLINT(google-explicit-constructor)
        : file_path(desc->file()->name()) {
      desc->GetLocationPath(&path);
    }
  };

  // Sink type for constructing values to pass to WithVars() and Emit().
  template <typename K, bool allow_callbacks>
  struct VarDefinition {
    using StringOrCallback = absl::variant<std::string, std::function<void()>>;

    template <typename Key, typename Value>
    VarDefinition(Key&& key, Value&& value)
        : key(std::forward<Key>(key)),
          value(ToStringOrCallback(std::forward<Value>(value), Rank2{})),
          annotation(absl::nullopt) {}

    // NOTE: This is an overload rather than taking optional<AnnotationRecord>
    // with a default argument of nullopt, because we want to pick up
    // AnnotationRecord's user-defined conversions. Because going from
    // e.g. Descriptor* -> optional<AnnotationRecord> requires two user-defined
    // conversions, this does not work.
    template <typename Key, typename Value>
    VarDefinition(Key&& key, Value&& value, AnnotationRecord annotation)
        : key(std::forward<Key>(key)),
          value(ToStringOrCallback(std::forward<Value>(value), Rank2{})),
          annotation(std::move(annotation)) {}

    K key;
    StringOrCallback value;
    absl::optional<AnnotationRecord> annotation;

   private:
    // go/ranked-overloads
    struct Rank0 {};
    struct Rank1 : Rank0 {};
    struct Rank2 : Rank1 {};

    // Dummy template for delayed instantiation, which is required for the
    // static assert below to kick in only when this function is called when it
    // shouldn't.
    //
    // This is done to produce a better error message than the "candidate does
    // not match" SFINAE errors.
    template <bool allowed = allow_callbacks>
    StringOrCallback ToStringOrCallback(std::function<void()> cb, Rank2) {
      static_assert(
          allowed, "callback-typed variables are not allowed in this location");
      return cb;
    }

    // Separate from the AlphaNum overload to avoid copies when taking strings
    // by value.
    StringOrCallback ToStringOrCallback(std::string s, Rank1) { return s; }

    StringOrCallback ToStringOrCallback(const absl::AlphaNum& s, Rank0) {
      return std::string(s.Piece());
    }
  };

  // Provide a helper to use heterogeneous lookup when it's available.
  template <class...>
  using void_t = void;
  template <typename Map, typename = void>
  struct has_heterogeneous_lookup : std::false_type {};
  template <typename Map>
  struct has_heterogeneous_lookup<Map, void_t<decltype(std::declval<Map>().find(
                                           std::declval<absl::string_view>()))>>
      : std::true_type {};

  template <typename Map,
            std::enable_if_t<has_heterogeneous_lookup<Map>::value, int> = 0>
  static absl::string_view ToStringKey(absl::string_view x) {
    return x;
  }
  template <typename Map,
            std::enable_if_t<!has_heterogeneous_lookup<Map>::value, int> = 0>
  static std::string ToStringKey(absl::string_view x) {
    return std::string(x);
  }

 public:
  static constexpr char kDefaultVariableDelimiter = '$';
  static constexpr absl::string_view kProtocCodegenTrace =
      "PROTOC_CODEGEN_TRACE";

  // Options for controlling how the output of a Printer is formatted.
  struct Options {
    Options() = default;
    Options(const Options&) = default;
    Options(Options&&) = default;
    Options(char variable_delimiter, AnnotationCollector* annotation_collector)
        : variable_delimiter(variable_delimiter),
          annotation_collector(annotation_collector) {}

    // The delimiter for variable substitutions, e.g. $foo$.
    char variable_delimiter = kDefaultVariableDelimiter;
    // An optional listener the Printer calls whenever it emits a source
    // annotation; may be null.
    AnnotationCollector* annotation_collector = nullptr;
    // The "comment start" token for the language being generated. This is used
    // to allow the Printer to emit debugging annotations in the source code
    // output.
    absl::string_view comment_start = "//";
    // The number of spaces that a single level of indentation adds by default;
    // this is the amount that WithIndent() increases indentation by.
    size_t spaces_per_indent = 2;
    // Whether to emit a "codegen trace" for calls to Emit(). If true, each call
    // to Emit() will print a comment indicating where in the source of the
    // compiler the Emit() call occurred.
    //
    // If disengaged, defaults to whether or not the environment variable
    // `PROTOC_CODEGEN_TRACE` is set.
    absl::optional<bool> enable_codegen_trace = absl::nullopt;
  };

  // Constructs a new Printer with the default options to output to
  // `output`.
  explicit Printer(ZeroCopyOutputStream* output) : Printer(output, Options{}) {}

  // Constructs a new printer with the given set of options to output to
  // `output`.
  Printer(ZeroCopyOutputStream* output, Options options);

  // Old-style constructor. Avoid in preference to the two constructors above.
  //
  // Will eventually be marked as deprecated.
  Printer(ZeroCopyOutputStream* output, char variable_delimiter,
          AnnotationCollector* annotation_collector = nullptr)
      : Printer(output, Options{variable_delimiter, annotation_collector}) {}

  Printer(const Printer&) = delete;
  Printer& operator=(const Printer&) = delete;

  // Pushes a new variable lookup frame that stores `vars` by reference.
  //
  // Returns an RAII object that pops the lookup frame.
  template <typename Map>
  auto WithVars(const Map* vars) {
    var_lookups_.emplace_back([vars](absl::string_view var) -> LookupResult {
      auto it = vars->find(ToStringKey<Map>(var));
      if (it == vars->end()) {
        return absl::nullopt;
      }
      return absl::string_view(it->second);
    });
    return absl::MakeCleanup([this] { var_lookups_.pop_back(); });
  }

  // Pushes a new variable lookup frame that stores `vars` by value.
  //
  // When writing `WithVars({...})`, this is the overload that will be called,
  // and it will synthesize an `absl::flat_hash_map`.
  //
  // Returns an RAII object that pops the lookup frame.
  template <typename Map = absl::flat_hash_map<std::string, std::string>,
            std::enable_if_t<!std::is_pointer<Map>::value, int> = 0>
  auto WithVars(Map&& vars) {
    var_lookups_.emplace_back([vars = std::forward<Map>(vars)](
                                  absl::string_view var) -> LookupResult {
      auto it = vars.find(ToStringKey<Map>(var));
      if (it == vars.end()) {
        return absl::nullopt;
      }
      return absl::string_view(it->second);
    });
    return absl::MakeCleanup([this] { var_lookups_.pop_back(); });
  }

  auto WithVars(std::initializer_list<
                VarDefinition<std::string, /*allow_callbacks=*/false>>
                    vars);

  // Looks up a variable set with WithVars().
  //
  // Will crash if:
  // - `var` is not present in the lookup frame table.
  // - `var` is a callback, rather than a string.
  absl::string_view LookupVar(absl::string_view var);

  // Pushes a new annotation lookup frame that stores `vars` by reference.
  //
  // Returns an RAII object that pops the lookup frame.
  template <typename Map>
  auto WithAnnotations(const Map* vars) {
    annotation_lookups_.emplace_back(
        [vars](absl::string_view var) -> absl::optional<AnnotationRecord> {
          auto it = vars->find(ToStringKey<Map>(var));
          if (it == vars->end()) {
            return absl::nullopt;
          }
          return AnnotationRecord(it->second);
        });
    return absl::MakeCleanup([this] { annotation_lookups_.pop_back(); });
  }

  // Pushes a new variable lookup frame that stores `vars` by value.
  //
  // When writing `WithAnnotations({...})`, this is the overload that will be
  // called, and it will synthesize an `absl::flat_hash_map`.
  //
  // Returns an RAII object that pops the lookup frame.
  template <typename Map = absl::flat_hash_map<std::string, AnnotationRecord>>
  auto WithAnnotations(Map&& vars) {
    annotation_lookups_.emplace_back(
        [vars = std::forward<Map>(vars)](
            absl::string_view var) -> absl::optional<AnnotationRecord> {
          auto it = vars.find(ToStringKey<Map>(var));
          if (it == vars.end()) {
            return absl::nullopt;
          }
          return AnnotationRecord(it->second);
        });
    return absl::MakeCleanup([this] { annotation_lookups_.pop_back(); });
  }

  // Increases the indentation by `indent` spaces; when nullopt, increments
  // indentation by the configured default spaces_per_indent.
  //
  // Returns an RAII object that removes this indentation.
  auto WithIndent(absl::optional<size_t> indent = absl::nullopt) {
    size_t delta = indent.value_or(options_.spaces_per_indent);
    indent_ += delta;
    return absl::MakeCleanup([this, delta] { indent_ -= delta; });
  }

  // Emits formatted source code to the underlying output. See the class
  // documentation for more details.
  //
  // `format` MUST be a string constant.
  void Emit(absl::string_view format,
            SourceLocation loc = SourceLocation::current()) {
    Emit({}, format, loc);
  }

  // Emits formatted source code to the underlying output, injecting
  // additional variables as a lookup frame for just this call. See the class
  // documentation for more details.
  //
  // `format` MUST be a string constant.
  void Emit(std::initializer_list<
                VarDefinition<absl::string_view, /*allow_callbacks=*/true>>
                vars,
            absl::string_view format,
            SourceLocation loc = SourceLocation::current());

  // Write a string directly to the underlying output, performing no formatting
  // of any sort.
  void PrintRaw(absl::string_view data) { WriteRaw(data.data(), data.size()); }

  // Write a string directly to the underlying output, performing no formatting
  // of any sort.
  void WriteRaw(const char* data, size_t size);

  // True if any write to the underlying stream failed.  (We don't just
  // crash in this case because this is an I/O failure, not a programming
  // error.)
  bool failed() const { return failed_; }

  // -- Old-style API below; to be deprecated and removed. --
  // TODO(b/242326974): Deprecate these APIs.

  template <typename Map = absl::flat_hash_map<std::string, std::string>>
  void Print(const Map& vars, absl::string_view text) {
    PrintOptions opts;
    opts.checks_are_debug_only = true;
    opts.use_substitution_map = true;
    opts.allow_digit_substitutions = false;

    auto pop = WithVars(&vars);
    PrintImpl(text, {}, opts);
  }

  template <typename... Args>
  void Print(absl::string_view text, const Args&... args) {
    static_assert(sizeof...(args) % 2 == 0, "");

    // Include an extra arg, since a zero-length array is ill-formed, and
    // MSVC complains.
    absl::string_view vars[] = {args..., ""};
    absl::flat_hash_map<absl::string_view, absl::string_view> map;
    map.reserve(sizeof...(args) / 2);
    for (size_t i = 0; i < sizeof...(args); i += 2) {
      map.emplace(vars[i], vars[i + 1]);
    }

    Print(map, text);
  }

  // Link a substitution variable emitted by the last call to Print to the
  // object described by descriptor.
  template <typename SomeDescriptor>
  void Annotate(absl::string_view varname, const SomeDescriptor* descriptor) {
    Annotate(varname, varname, descriptor);
  }

  // Link the output range defined by the substitution variables as emitted by
  // the last call to Print to the object described by descriptor. The range
  // begins at begin_varname's value and ends after the last character of the
  // value substituted for end_varname.
  template <typename SomeDescriptor>
  void Annotate(absl::string_view begin_varname, absl::string_view end_varname,
                const SomeDescriptor* descriptor) {
    if (options_.annotation_collector == nullptr) {
      return;
    }

    std::vector<int> path;
    descriptor->GetLocationPath(&path);
    Annotate(begin_varname, end_varname, descriptor->file()->name(), path);
  }

  // Link a substitution variable emitted by the last call to Print to the file
  // with path file_name.
  void Annotate(absl::string_view varname, absl::string_view file_name) {
    Annotate(varname, varname, file_name);
  }

  // Link the output range defined by the substitution variables as emitted by
  // the last call to Print to the file with path file_name. The range begins
  // at begin_varname's value and ends after the last character of the value
  // substituted for end_varname.
  void Annotate(absl::string_view begin_varname, absl::string_view end_varname,
                absl::string_view file_name) {
    if (options_.annotation_collector == nullptr) {
      return;
    }

    Annotate(begin_varname, end_varname, file_name, {});
  }

  // Indent text by `options.spaces_per_indent`; undone by Outdent().
  void Indent() { indent_ += options_.spaces_per_indent; }

  // Undoes a call to Indent().
  void Outdent();

  // FormatInternal is a helper function not meant to use directly, use
  // compiler::cpp::Formatter instead.
  template <typename Map = absl::flat_hash_map<std::string, std::string>>
  void FormatInternal(absl::Span<const std::string> args, const Map& vars,
                      absl::string_view format) {
    PrintOptions opts;
    opts.use_curly_brace_substitutions = true;
    opts.strip_spaces_around_vars = true;

    auto pop = WithVars(&vars);
    PrintImpl(format, args, opts);
  }

 private:
  // Options for PrintImpl().
  struct PrintOptions {
    // The callsite of the public entry-point. Only Emit() sets this.
    absl::optional<SourceLocation> loc;
    // If set, Validate() calls will not crash the program.
    bool checks_are_debug_only = false;
    // If set, the `substitutions_` map will be populated as variables are
    // substituted.
    bool use_substitution_map = false;
    // If set, the ${1$ and $}$ forms will be substituted. These are used for
    // a slightly janky annotation-insertion mechanism in FormatInternal, that
    // requires that passed-in substitution variables be serialized protos.
    bool use_curly_brace_substitutions = false;
    // If set, the $n$ forms will be substituted, pulling from the `args`
    // argument to PrintImpl().
    bool allow_digit_substitutions = true;
    // If set, when a variable substitution with spaces in it, such as $ var$,
    // is encountered, the spaces are stripped, so that it is as if it was
    // $var$. If $var$ substitutes to a non-empty string, the removed spaces are
    // printed around the substituted value.
    //
    // See the class documentation for more information on this behavior.
    bool strip_spaces_around_vars = true;
    // If set, leading whitespace will be stripped from the format string to
    // determine the "extraneous indentation" that is produced when the format
    // string is a C++ raw string. This is used to remove leading spaces from
    // a raw string that would otherwise result in erratic indentation in the
    // output.
    bool strip_raw_string_indentation = false;
    // If set, the annotation lookup frames are searched, per the annotation
    // semantics of Emit() described in the class documentation.
    bool use_annotation_frames = true;
  };

  // Emit an annotation for the range defined by the given substitution
  // variables, as set by the most recent call to PrintImpl() that set
  // `use_substitution_map` to true.
  //
  // The range begins at the start of `begin_varname`'s value and ends after the
  // last byte of `end_varname`'s value.
  //
  // `begin_varname` and `end_varname may` refer to the same variable.
  void Annotate(absl::string_view begin_varname, absl::string_view end_varname,
                absl::string_view file_path, const std::vector<int>& path);

  // The core printing implementation. There are three public entry points,
  // which enable different slices of functionality that are controlled by the
  // `opts` argument.
  void PrintImpl(absl::string_view format, absl::Span<const std::string> args,
                 PrintOptions opts);

  // This is a private function only so that it can see PrintOptions.
  static bool Validate(bool cond, PrintOptions opts,
                       absl::FunctionRef<std::string()> message);
  static bool Validate(bool cond, PrintOptions opts, absl::string_view message);

  // Performs calls to `Validate()` to check that `index < current_arg_index`
  // and `index < args_len`, producing appropriate log lines if the checks fail,
  // and crashing if necessary.
  bool ValidateIndexLookupInBounds(size_t index, size_t current_arg_index,
                                   size_t args_len, PrintOptions opts);

  // Prints indentation if `at_start_of_line_` is true.
  void IndentIfAtStart();

  // Prints a codegen trace, for the given location in the compiler's source.
  void PrintCodegenTrace(absl::optional<SourceLocation> loc);

  // The core implementation for "fully-elaborated" variable definitions. This
  // is a private function to avoid users being able to set `allow_callbacks`.
  template <typename K, bool allow_callbacks>
  auto WithDefs(std::initializer_list<VarDefinition<K, allow_callbacks>> vars);

  // Returns the start and end of the value that was substituted in place of
  // the variable `varname` in the last call to PrintImpl() (with
  // `use_substitution_map` set), if such a variable was substituted exactly
  // once.
  absl::optional<std::pair<size_t, size_t>> GetSubstitutionRange(
      absl::string_view varname, PrintOptions opts);

  google::protobuf::io::zc_sink_internal::ZeroCopyStreamByteSink sink_;
  Options options_;
  size_t indent_ = 0;
  bool at_start_of_line_ = true;
  bool failed_ = false;

  using LookupResult =
      absl::optional<absl::variant<absl::string_view, std::function<void()>>>;

  std::vector<std::function<LookupResult(absl::string_view)>> var_lookups_;

  std::vector<
      std::function<absl::optional<AnnotationRecord>(absl::string_view)>>
      annotation_lookups_;

  // A map from variable name to [start, end) offsets in the output buffer.
  //
  // This stores the data looked up by GetSubstitutionRange().
  std::map<std::string, std::pair<size_t, size_t>> substitutions_;
  // Keeps track of the keys in `substitutions_` that need to be updated when
  // indents are inserted. These are keys that refer to the beginning of the
  // current line.
  std::vector<std::string> line_start_variables_;
};

template <typename K, bool allow_callbacks>
auto Printer::WithDefs(
    std::initializer_list<VarDefinition<K, allow_callbacks>> vars) {
  absl::flat_hash_map<K, absl::variant<std::string, std::function<void()>>>
      var_map;
  var_map.reserve(vars.size());

  absl::flat_hash_map<K, AnnotationRecord> annotation_map;

  for (auto& var : vars) {
    auto result = var_map.insert({var.key, var.value});
    GOOGLE_CHECK(result.second) << "repeated variable in Emit() or WithVars() call: \""
                         << var.key << "\"";
    if (var.annotation.has_value()) {
      annotation_map.insert({var.key, *var.annotation});
    }
  }

  var_lookups_.emplace_back(
      [map = std::move(var_map)](absl::string_view var) -> LookupResult {
        auto it = map.find(var);
        if (it == map.end()) {
          return absl::nullopt;
        }
        if (auto* str = absl::get_if<std::string>(&it->second)) {
          return absl::string_view(*str);
        }

        auto* f = absl::get_if<std::function<void()>>(&it->second);
        GOOGLE_CHECK(f != nullptr);
        return *f;
      });

  bool has_annotations = !annotation_map.empty();
  if (has_annotations) {
    annotation_lookups_.emplace_back(
        [map = std::move(annotation_map)](
            absl::string_view var) -> absl::optional<AnnotationRecord> {
          auto it = map.find(var);
          if (it == map.end()) {
            return absl::nullopt;
          }
          return it->second;
        });
  }

  return absl::MakeCleanup([this, has_annotations] {
    var_lookups_.pop_back();
    if (has_annotations) {
      annotation_lookups_.pop_back();
    }
  });
}

inline auto Printer::WithVars(
    std::initializer_list<VarDefinition<std::string, /*allow_callbacks=*/false>>
        vars) {
  return WithDefs(vars);
}
}  // namespace io
}  // namespace protobuf
}  // namespace google

#include "google/protobuf/port_undef.inc"

#endif  // GOOGLE_PROTOBUF_IO_PRINTER_H__
