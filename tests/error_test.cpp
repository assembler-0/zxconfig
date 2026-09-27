// SPDX-License-Identifier: Apache-2.0

#include <zxconfig/error.hpp>

#include <gtest/gtest.h>

using zxconfig::ErrorCode;
using zxconfig::ErrorDomain;

TEST(ErrorCode, ranges_map_to_their_domain) {
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::Ok), ErrorDomain::None);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::FileNotFound), ErrorDomain::Io);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::UnexpectedCharacter), ErrorDomain::Lex);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::UnexpectedToken), ErrorDomain::Parse);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::DuplicateSymbol), ErrorDomain::Semantic);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::RangeViolated), ErrorDomain::Validation);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::ProbeFailed), ErrorDomain::Evaluation);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::WriteFailed), ErrorDomain::Generation);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::CacheCorrupt), ErrorDomain::Config);
    EXPECT_EQ(zxconfig::domain_of(ErrorCode::Invariant), ErrorDomain::Internal);
}

TEST(ErrorCode, every_code_has_a_stable_name) {
    const ErrorCode codes[] = {
        ErrorCode::Ok,
        ErrorCode::FileNotFound, ErrorCode::PermissionDenied, ErrorCode::IoFailure,
        ErrorCode::PathEscapesRoot, ErrorCode::FileTooLarge, ErrorCode::EncodingInvalid,
        ErrorCode::DirectoryExpected, ErrorCode::AlreadyLoaded,
        ErrorCode::UnexpectedCharacter, ErrorCode::UnterminatedString,
        ErrorCode::UnterminatedComment, ErrorCode::BadNumber, ErrorCode::BadEscape,
        ErrorCode::UnexpectedToken, ErrorCode::ExpectedToken, ErrorCode::MissingVersion,
        ErrorCode::UnsupportedVersion, ErrorCode::UnbalancedDelimiter,
        ErrorCode::UnsupportedConstruct, ErrorCode::UnknownBuiltin,
        ErrorCode::UnknownSymbol, ErrorCode::DuplicateSymbol, ErrorCode::TypeMismatch,
        ErrorCode::CircularDependency, ErrorCode::UnknownType, ErrorCode::NamespaceCollision,
        ErrorCode::UndeclaredMacro, ErrorCode::ArityMismatch, ErrorCode::DeprecatedSymbol,
        ErrorCode::UnknownChoice,
        ErrorCode::ConstraintViolated, ErrorCode::RangeViolated, ErrorCode::PatternMismatch,
        ErrorCode::PathConstraint, ErrorCode::ConflictUnresolved,
        ErrorCode::DivisionByZero, ErrorCode::ProbeFailed, ErrorCode::Interpolation,
        ErrorCode::UndefinedValue,
        ErrorCode::UnknownBackend, ErrorCode::TemplateMissing, ErrorCode::WriteFailed,
        ErrorCode::BadInvocation, ErrorCode::CacheCorrupt, ErrorCode::CacheMissing,
        ErrorCode::CacheStale, ErrorCode::CacheUnsupported, ErrorCode::LockBusy,
        ErrorCode::Unreachable, ErrorCode::Invariant,
    };
    for (const auto code : codes) {
        const auto name = zxconfig::to_string(code);
        EXPECT_STRNE(name, "unknown") << static_cast<unsigned>(code);
        EXPECT_EQ(std::string{name}.find(' '), std::string::npos);
    }
}

TEST(Diagnostic, factories_set_severity) {
    const auto err = zxconfig::Diagnostic::error(ErrorCode::IoFailure, "disk");
    EXPECT_EQ(err.severity, zxconfig::Severity::Error);
    EXPECT_EQ(err.code, ErrorCode::IoFailure);

    const auto fatal = zxconfig::Diagnostic::fatal(ErrorCode::Invariant, "broken");
    EXPECT_EQ(fatal.severity, zxconfig::Severity::Fatal);

    const auto warn = zxconfig::Diagnostic::warning(ErrorCode::DeprecatedSymbol, "old");
    EXPECT_EQ(warn.severity, zxconfig::Severity::Warning);

    EXPECT_FALSE(err.span.is_valid());
}

TEST(SourceSpan, point_is_zero_width) {
    const zxconfig::SourceLocation loc{3, 40};
    const auto span = zxconfig::SourceSpan::point(loc);
    EXPECT_EQ(span.begin, span.end);
    EXPECT_TRUE(span.is_valid());
    EXPECT_FALSE(zxconfig::SourceLocation{}.is_valid());
}
