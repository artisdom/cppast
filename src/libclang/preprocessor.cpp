// Copyright (C) 2017 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include "preprocessor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cctype>

#include <process.hpp>

#include <type_safe/flag.hpp>
#include <type_safe/optional.hpp>
#include <type_safe/reference.hpp>

#include <cppast/cpp_entity_kind.hpp>
#include <cppast/diagnostic.hpp>

#include "parse_error.hpp"

using namespace cppast;
namespace ts = type_safe;

namespace
{
    std::string quote(std::string str)
    {
        return '"' + std::move(str) + '"';
    }

    bool support_include(const libclang_compile_config& c)
    {
        return detail::libclang_compile_config_access::clang_version(c) >= 40000;
    }

    // build the command that runs the preprocessor
    std::string get_command(const libclang_compile_config& c, const char* full_path)
    {
        // -x c++: force C++ as input language
        // -I.: add current working directory to include search path
        // -E: print preprocessor output
        // -C: keep comments
        // -dD: print macro definitions as well
        auto flags = std::string("-x c++ -I. -E -C -dD");
        if (support_include(c))
            // -Xclang -dI: print include directives as well (clang >= 4.0.0)
            flags += " -Xclang -dI";
        // -fno-caret-diagnostics: don't show the source extract in diagnostics
        // -fno-show-column: don't show the column number
        // -fdiagnostics-format msvc: use easier to parse MSVC format
        flags += " -fno-caret-diagnostics -fno-show-column -fdiagnostics-format=msvc";
        // -Wno-*: hide wrong warnings if header file is directly parsed
        flags += " -Wno-pragma-once-outside-header -Wno-pragma-system-header-outside-header "
                 "-Wno-include-next-outside-header";

        std::string cmd(detail::libclang_compile_config_access::clang_binary(c) + " "
                        + std::move(flags) + " ");

        // add other flags
        for (auto& flag : detail::libclang_compile_config_access::flags(c))
        {
            cmd += flag;
            cmd += ' ';
        }

        // add path to file being processed
        cmd += quote(full_path);

        return cmd;
    }

    source_location parse_source_location(const char*& ptr)
    {
        // format: <filename>(<line>):
        // or: <filename>:
        auto        fallback = ptr;
        std::string filename;
        while (*ptr && *ptr != ':' && *ptr != '(')
            filename.push_back(*ptr++);

        if (filename == "error" || filename == "warning" || filename == "fatal error")
        {
            ptr = fallback;
            return {};
        }

        type_safe::optional<unsigned> line;
        if (*ptr == '(')
        {
            ++ptr;

            std::string str;
            while (*ptr != ')')
                str.push_back(*ptr++);
            ++ptr;

            line = unsigned(std::stoi(str));
        }

        DEBUG_ASSERT(*ptr == ':', detail::assert_handler{});
        ++ptr;

        return {type_safe::nullopt, std::move(filename), std::move(line), type_safe::nullopt};
    }

    severity parse_severity(const char*& ptr)
    {
        // format: <severity>:
        auto        fallback = ptr;
        std::string sev;
        while (*ptr && *ptr != ':')
            sev.push_back(*ptr++);
        ++ptr;

        if (sev == "warning")
            return severity::warning;
        else if (sev == "error")
            return severity::error;
        else if (sev == "fatal error")
            return severity::critical;
        else
            ptr = fallback;
        return severity::error;
    }

    // parse and log diagnostic
    void log_diagnostic(const diagnostic_logger& logger, const std::string& msg)
    {
        auto ptr = msg.c_str();

        auto loc = parse_source_location(ptr);
        while (*ptr == ' ')
            ++ptr;

        auto sev = parse_severity(ptr);
        while (*ptr == ' ')
            ++ptr;

        std::string message;
        while (*ptr && *ptr != '\n')
            message.push_back(*ptr++);

        logger.log("preprocessor", diagnostic{std::move(message), std::move(loc), sev});
    }

    // gets the full preprocessor output
    std::string get_full_preprocess_output(const libclang_compile_config& c, const char* full_path,
                                           const diagnostic_logger& logger)
    {
        namespace tpl = TinyProcessLib;

        std::string preprocessed, diagnostic;

        auto         cmd = get_command(c, full_path);
        tpl::Process process(cmd, "",
                             [&](const char* str, std::size_t n) {
                                 preprocessed.reserve(preprocessed.size() + n);
                                 for (auto end = str + n; str != end; ++str)
                                     if (*str == '\t')
                                         preprocessed += "  "; // just two spaces because why not
                                     else if (*str != '\r')
                                         preprocessed.push_back(*str);
                             },
                             [&](const char* str, std::size_t n) {
                                 diagnostic.reserve(diagnostic.size() + n);
                                 for (auto end = str + n; str != end; ++str)
                                     if (*str == '\r')
                                         continue;
                                     else if (*str == '\n')
                                     {
                                         // consume current diagnostic
                                         log_diagnostic(logger, diagnostic);
                                         diagnostic.clear();
                                     }
                                     else
                                         diagnostic.push_back(*str);
                             });

        auto exit_code = process.get_exit_status();
        DEBUG_ASSERT(diagnostic.empty(), detail::assert_handler{});
        if (exit_code != 0)
            throw libclang_error("preprocessor: command '" + cmd
                                 + "' exited with non-zero exit code (" + std::to_string(exit_code)
                                 + ")");

        return preprocessed;
    }

    class position
    {
    public:
        position(ts::object_ref<std::string> result, const char* ptr) noexcept
        : result_(result), cur_line_(1u), ptr_(ptr), write_(true)
        {
        }

        void set_line(unsigned line)
        {
            if (cur_line_ != line)
            {
                *result_ += "#line " + std::to_string(line) + "\n";
                cur_line_   = line;
                cur_column_ = 0;
            }
        }

        void write_str(std::string str)
        {
            if (write_ == false)
                return;
            for (auto c : str)
            {
                *result_ += c;
                ++cur_column_;

                if (c == '\n')
                {
                    ++cur_line_;
                    cur_column_ = 0;
                }
            }
        }

        void bump() noexcept
        {
            if (write_ == true)
            {
                result_->push_back(*ptr_);
                ++cur_column_;

                if (*ptr_ == '\n')
                {
                    ++cur_line_;
                    cur_column_ = 0;
                }
            }
            ++ptr_;
        }

        void bump(std::size_t offset) noexcept
        {
            if (write_ == true)
            {
                for (std::size_t i = 0u; i != offset; ++i)
                    bump();
            }
            else
                skip(offset);
        }

        // no write, no newline detection
        void skip(std::size_t offset = 1u) noexcept
        {
            ptr_ += offset;
        }

        void skip_with_linecount() noexcept
        {
            if (write_ == true)
            {
                ++cur_column_;
                if (*ptr_ == '\n')
                {
                    result_->push_back('\n');
                    ++cur_line_;
                    cur_column_ = 0;
                }
            }

            ++ptr_;
        }

        void enable_write() noexcept
        {
            write_.set();
        }

        void disable_write() noexcept
        {
            write_.try_reset();
        }

        explicit operator bool() const noexcept
        {
            return *ptr_ != '\0';
        }

        const char* ptr() const noexcept
        {
            return ptr_;
        }

        unsigned cur_line() const noexcept
        {
            return cur_line_;
        }

        unsigned cur_column() const noexcept
        {
            return cur_column_;
        }

        bool was_newl() const noexcept
        {
            return result_->empty() || result_->back() == '\n';
        }

    private:
        ts::object_ref<std::string> result_;
        unsigned                    cur_line_, cur_column_;
        const char*                 ptr_;
        ts::flag                    write_;
    };

    bool starts_with(const position& p, const char* str, std::size_t len)
    {
        return std::strncmp(p.ptr(), str, len) == 0;
    }

    template <std::size_t N>
    bool starts_with(const position& p, const char (&str)[N])
    {
        return std::strncmp(p.ptr(), str, N - 1) == 0;
    }

    void skip(position& p, const char* str)
    {
        DEBUG_ASSERT(starts_with(p, str, std::strlen(str)), detail::assert_handler{});
        p.skip(std::strlen(str));
    }

    void skip_spaces(position& p, bool bump = false)
    {
        while (starts_with(p, " "))
            if (bump)
                p.bump();
            else
                p.skip();
    }

    struct linemarker
    {
        std::string file;
        unsigned    line;
        enum
        {
            line_directive, // no change in file
            enter_new,      // open a new file
            enter_old,      // return to an old file
        } flag         = line_directive;
        bool is_system = false;
    };

    ts::optional<linemarker> parse_linemarker(position& p)
    {
        // format (at new line): # <line> "<filename>" <flags>
        // flag 1: enter_new
        // flag 2: enter_old
        // flag 3: system file
        // flag 4: ignored
        if (!p.was_newl() || !starts_with(p, "#"))
            return ts::nullopt;
        p.skip();
        DEBUG_ASSERT(!starts_with(p, "define") && !starts_with(p, "undef")
                         && !starts_with(p, "pragma"),
                     detail::assert_handler{}, "handle macros first");

        linemarker result;

        std::string line;
        for (skip_spaces(p); std::isdigit(*p.ptr()); p.skip())
            line += *p.ptr();
        result.line = unsigned(std::stoi(line));

        skip_spaces(p);
        DEBUG_ASSERT(*p.ptr() == '"', detail::assert_handler{});
        p.skip();

        std::string file_name;
        for (; !starts_with(p, "\""); p.skip())
            file_name += *p.ptr();
        p.skip();
        result.file = std::move(file_name);

        for (; !starts_with(p, "\n"); p.skip())
        {
            skip_spaces(p);

            switch (*p.ptr())
            {
            case '1':
                DEBUG_ASSERT(result.flag == linemarker::line_directive, detail::assert_handler{});
                result.flag = linemarker::enter_new;
                break;
            case '2':
                DEBUG_ASSERT(result.flag == linemarker::line_directive, detail::assert_handler{});
                result.flag = linemarker::enter_old;
                break;
            case '3':
                result.is_system = true;
                break;
            case '4':
                break; // ignored

            default:
                DEBUG_UNREACHABLE(detail::assert_handler{}, "invalid line marker");
                break;
            }
        }
        skip(p, "\n");

        return result;
    }

    detail::pp_doc_comment parse_c_doc_comment(position& p)
    {
        detail::pp_doc_comment result;
        result.kind = detail::pp_doc_comment::c;

        auto indent = p.cur_column() + 3;

        if (starts_with(p, " "))
        {
            // skip one whitespace at most
            p.skip();
            ++indent;
        }

        while (!starts_with(p, "*/"))
        {
            if (starts_with(p, "\n"))
            {
                // remove trailing spaces
                while (!result.comment.empty() && result.comment.back() == ' ')
                    result.comment.pop_back();

                // skip newline
                p.skip_with_linecount();
                result.comment += '\n';

                // skip indentation
                for (auto i = 0u; i < indent && starts_with(p, " "); ++i)
                    p.skip();

                auto extra_indent = 0u;
                while (starts_with(p, " "))
                {
                    ++extra_indent;
                    p.skip();
                }

                // skip continuation star, if any
                if (starts_with(p, "*") && !starts_with(p, "*/"))
                {
                    p.skip();
                    if (starts_with(p, " "))
                        // skip one whitespace at most
                        p.skip();
                }
                else
                {
                    // insert extra indent again
                    result.comment += std::string(extra_indent, ' ');
                }
            }
            else
            {
                result.comment += *p.ptr();
                p.skip();
            }
        }
        p.skip(2u);

        // remove trailing star
        if (!result.comment.empty() && result.comment.back() == '*')
            result.comment.pop_back();
        // remove trailing spaces
        while (!result.comment.empty() && result.comment.back() == ' ')
            result.comment.pop_back();

        result.line = p.cur_line();
        return result;
    }

    bool skip_c_comment(position& p, detail::preprocessor_output& output, bool in_main_file)
    {
        if (!starts_with(p, "/*"))
            return false;
        p.skip(2u);

        if (starts_with(p, "*/"))
            // empty comment
            p.skip(2u);
        else if (in_main_file && (starts_with(p, "*") || starts_with(p, "!")))
        {
            // doc comment
            p.skip();
            output.comments.push_back(parse_c_doc_comment(p));
        }
        else
        {
            while (!starts_with(p, "*/"))
                p.skip_with_linecount();
            p.skip(2u);
        }

        return true;
    }

    detail::pp_doc_comment parse_cpp_doc_comment(position& p, bool end_of_line)
    {
        detail::pp_doc_comment result;
        result.kind =
            end_of_line ? detail::pp_doc_comment::end_of_line : detail::pp_doc_comment::cpp;
        if (starts_with(p, " "))
            // skip one whitespace at most
            p.skip();

        while (!starts_with(p, "\n"))
        {
            result.comment += *p.ptr();
            p.skip();
        }
        // don't skip newline

        // remove trailing spaces
        while (!result.comment.empty() && result.comment.back() == ' ')
            result.comment.pop_back();
        result.line = p.cur_line();
        return result;
    }

    bool can_merge_comment(const detail::pp_doc_comment& comment, unsigned cur_line)
    {
        return comment.line + 1 == cur_line
               && (comment.kind == detail::pp_doc_comment::cpp
                   || comment.kind == detail::pp_doc_comment::end_of_line);
    }

    void merge_or_add(detail::preprocessor_output& output, detail::pp_doc_comment comment)
    {
        if (output.comments.empty() || !can_merge_comment(output.comments.back(), comment.line))
            output.comments.push_back(std::move(comment));
        else
        {
            auto& result = output.comments.back();
            result.comment += "\n" + std::move(comment.comment);
            if (result.kind != detail::pp_doc_comment::end_of_line)
                result.line = comment.line;
        }
    }

    bool skip_cpp_comment(position& p, detail::preprocessor_output& output, bool in_main_file)
    {
        if (!starts_with(p, "//"))
            return false;
        p.skip(2u);

        if (in_main_file && (starts_with(p, "/") || starts_with(p, "!")))
        {
            // C++ style doc comment
            p.skip();
            auto comment = parse_cpp_doc_comment(p, false);
            merge_or_add(output, std::move(comment));
        }
        else if (in_main_file && starts_with(p, "<"))
        {
            // end of line doc comment
            p.skip();
            auto comment = parse_cpp_doc_comment(p, true);
            output.comments.push_back(std::move(comment));
        }
        else
        {
            auto newline = std::strchr(p.ptr(), '\n');
            p.skip(std::size_t(newline - p.ptr())); // don't skip newline
        }

        return true;
    }

    std::unique_ptr<cpp_macro_definition> parse_macro(position&                    p,
                                                      detail::preprocessor_output& output,
                                                      bool                         in_main_file)
    {
        // format (at new line): #define <name> [replacement]
        // or: #define <name>(<args>) [replacement]
        // note: keep macro definition in file
        if (!p.was_newl() || !starts_with(p, "#define"))
            return nullptr;
        // read line here for comment matching
        auto cur_line = p.cur_line();
        p.bump(std::strlen("#define"));
        skip_spaces(p, true);

        std::string name;
        while (!starts_with(p, "(") && !starts_with(p, " ") && !starts_with(p, "\n"))
        {
            name += *p.ptr();
            p.bump();
        }

        ts::optional<std::string> args;
        if (starts_with(p, "("))
        {
            std::string str;
            for (p.bump(); !starts_with(p, ")"); p.bump())
                str += *p.ptr();
            p.bump();
            args = std::move(str);
        }

        std::string rep;
        auto        in_c_comment = false;
        for (skip_spaces(p, true); in_c_comment || !starts_with(p, "\n"); p.bump())
        {
            if (starts_with(p, "/*"))
                in_c_comment = true;
            else if (in_c_comment && starts_with(p, "*/"))
                in_c_comment = false;
            rep += *p.ptr();
        }
        // don't skip newline

        if (!in_main_file)
            return nullptr;

        auto result = cpp_macro_definition::build(std::move(name), std::move(args), std::move(rep));
        // match comment directly
        if (!output.comments.empty() && output.comments.back().matches(*result, cur_line))
        {
            result->set_comment(std::move(output.comments.back().comment));
            output.comments.pop_back();
        }
        return result;
    }

    ts::optional<std::string> parse_undef(position& p)
    {
        // format (at new line): #undef <name>
        // due to a clang bug (http://bugs.llvm.org/show_bug.cgi?id=32631) I'll also an undef in the middle of the line
        if (/*!p.was_newl() ||*/ !starts_with(p, "#undef"))
            return ts::nullopt;
        p.bump(std::strlen("#undef"));

        std::string result;
        for (skip_spaces(p, true); !starts_with(p, "\n"); p.bump())
            result += *p.ptr();
        // don't skip newline

        return result;
    }

    type_safe::optional<detail::pp_include> parse_include(position& p, bool in_main_file)
    {
        // format (at new line, literal <>): #include <filename>
        // or: #include "filename"
        // note: don't write include back
        if (!p.was_newl() || !starts_with(p, "#include"))
            return type_safe::nullopt;
        p.skip(std::strlen("#include"));
        if (starts_with(p, "_next"))
            p.skip(std::strlen("_next"));
        skip_spaces(p);

        auto include_kind = cpp_include_kind::system;
        auto end_str      = "";
        if (starts_with(p, "\""))
        {
            include_kind = cpp_include_kind::local;
            end_str      = "\"";
        }
        else if (starts_with(p, "<"))
        {
            include_kind = cpp_include_kind::system;
            end_str      = ">";
        }
        else
            DEBUG_UNREACHABLE(detail::assert_handler{});
        p.skip();

        std::string filename;
        for (; !starts_with(p, "\"") && !starts_with(p, ">"); p.skip())
            filename += *p.ptr();
        DEBUG_ASSERT(starts_with(p, end_str, std::strlen(end_str)), detail::assert_handler{},
                     "bad termination");
        p.skip();
        skip(p, " /* clang -E -dI */");
        DEBUG_ASSERT(starts_with(p, "\n"), detail::assert_handler{});
        // don't skip newline

        if (!in_main_file)
            return type_safe::nullopt;

        return detail::pp_include{cpp_file_ref(cpp_entity_id(filename), filename), include_kind,
                                  p.cur_line()};
    }

    bool bump_pragma(position& p)
    {
        // format (at new line): #pragma <stuff..>\n
        if (!p.was_newl() || !starts_with(p, "#pragma"))
            return false;

        while (!starts_with(p, "\n"))
            p.bump();
        // don't skip newline

        return true;
    }
}

detail::preprocessor_output detail::preprocess(const libclang_compile_config& config,
                                               const char* path, const diagnostic_logger& logger)
{
    detail::preprocessor_output result;

    auto output = get_full_preprocess_output(config, path, logger);

    position    p(ts::ref(result.source), output.c_str());
    std::size_t file_depth = 0u;
    ts::flag    in_string(false), in_char(false);
    while (p)
    {
        auto next = std::strpbrk(p.ptr(), "\"'#/"); // look for ", ', # or /
        if (next && next > p.ptr())
            p.bump(std::size_t(next - p.ptr() - 1)); // subtract one to get before that character

        if (starts_with(p, "\\\"")) // starts with \"
            p.bump(2u);
        else if (starts_with(p, "\\'")) // starts with \'
            p.bump(2u);
        else if (in_char == false && starts_with(p, "\"")) // starts with "
        {
            p.bump();
            in_string.toggle();
        }
        else if (in_string == false && starts_with(p, "'"))
        {
            p.bump();
            in_char.toggle();
        }
        else if (in_string == true || in_char == true)
            p.bump();
        else if (auto macro = parse_macro(p, result, file_depth == 0u))
        {
            if (logger.is_verbose())
                logger.log("preprocessor",
                           format_diagnostic(severity::debug,
                                             source_location::make_file(path, p.cur_line()),
                                             "parsing macro '", macro->name(), "'"));

            result.macros.push_back({std::move(macro), p.cur_line()});
        }
        else if (auto undef = parse_undef(p))
        {
            if (file_depth == 0u)
            {
                if (logger.is_verbose())
                    logger.log("preprocessor",
                               format_diagnostic(severity::debug,
                                                 source_location::make_file(path, p.cur_line()),
                                                 "undefining macro '", undef.value(), "'"));

                result.macros.erase(std::remove_if(result.macros.begin(), result.macros.end(),
                                                   [&](const pp_macro& e) {
                                                       return e.macro->name() == undef.value();
                                                   }),
                                    result.macros.end());
            }
        }
        else if (auto include = parse_include(p, file_depth == 0u))
        {
            if (logger.is_verbose())
                logger.log("preprocessor",
                           format_diagnostic(severity::debug,
                                             source_location::make_file(path, p.cur_line()),
                                             "parsing include '", include.value().file.name(),
                                             "'"));

            result.includes.push_back(std::move(include.value()));
        }
        else if (bump_pragma(p))
            continue;
        else if (auto lm = parse_linemarker(p))
        {
            switch (lm.value().flag)
            {
            case linemarker::line_directive:
                if (file_depth == 0u)
                    p.set_line(lm.value().line);
                break;

            case linemarker::enter_new:
                if (file_depth == 0u && lm.value().file.front() != '<')
                {
                    // this file is directly included by the given file
                    // and it is not a fake file like builtin or command line

                    // write include with full path
                    p.write_str("#include \"" + lm.value().file + "\"\n");
                    // note: don't build include here, do it when an #include is encountered
                }

                ++file_depth;
                p.disable_write();
                break;

            case linemarker::enter_old:
                --file_depth;
                if (file_depth == 0u)
                {
                    DEBUG_ASSERT(lm.value().file == path, detail::assert_handler{});
                    p.set_line(lm.value().line);
                    p.enable_write();
                }
                break;
            }
        }
        else if (skip_c_comment(p, result, file_depth == 0u))
            continue;
        else if (skip_cpp_comment(p, result, file_depth == 0u))
            continue;
        else
            p.bump();
    }

    return result;
}

bool detail::pp_doc_comment::matches(const cpp_entity&, unsigned e_line)
{
    if (kind == detail::pp_doc_comment::end_of_line)
        return line == e_line;
    else
        return line + 1u == e_line;
}
