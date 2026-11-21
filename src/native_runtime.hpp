#pragma once

#include <ostream>
#include <string>

namespace veloz::native {

inline void writeRuntime(std::ostream& out) {
    out << R"ll(
%text = type { ptr, i64 }
declare i64 @fwrite(ptr, i64, i64, ptr)
declare i32 @putchar(i32)
declare i32 @getchar()
declare i32 @memcmp(ptr, ptr, i64)
declare void @exit(i32) noreturn
declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)
declare {i64, i1} @llvm.ssub.with.overflow.i64(i64, i64)
declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64)

)ll";
#ifdef _WIN32
    out << R"ll(declare ptr @__acrt_iob_func(i32)
declare i32 @_setmode(i32, i32)
define internal void @tz.init() sanitize_address {
  call i32 @_setmode(i32 0, i32 32768)
  call i32 @_setmode(i32 1, i32 32768)
  call i32 @_setmode(i32 2, i32 32768)
  ret void
}
define internal ptr @tz.stream(i1 %error) sanitize_address {
  %index = select i1 %error, i32 2, i32 1
  %stream = call ptr @__acrt_iob_func(i32 %index)
  ret ptr %stream
}
)ll";
#else
#ifdef __APPLE__
    constexpr auto stdoutName = "@__stdoutp";
    constexpr auto stderrName = "@__stderrp";
#else
    constexpr auto stdoutName = "@stdout";
    constexpr auto stderrName = "@stderr";
#endif
    out << stdoutName << " = external global ptr\n" << stderrName << " = external global ptr\n"
        << "define internal void @tz.init() sanitize_address { ret void }\n"
        << "define internal ptr @tz.stream(i1 %error) sanitize_address {\n"
        << "  %ref = select i1 %error, ptr " << stderrName << ", ptr " << stdoutName
        << "\n  %stream = load ptr, ptr %ref\n  ret ptr %stream\n}\n";
#endif
    out << R"ll(
define internal void @tz.write(%text %s, i1 %error) sanitize_address {
  %p = extractvalue %text %s, 0
  %n = extractvalue %text %s, 1
  %stream = call ptr @tz.stream(i1 %error)
  call i64 @fwrite(ptr %p, i64 1, i64 %n, ptr %stream)
  ret void
}
define internal void @tz.guard(i1 %bad, %text %message) sanitize_address {
  br i1 %bad, label %trap, label %ok
trap:
  call void @tz.write(%text %message, i1 true)
  call void @exit(i32 1)
  unreachable
ok:
  ret void
}
define internal i1 @tz.eq(%text %a, %text %b) sanitize_address {
  %na = extractvalue %text %a, 1
  %nb = extractvalue %text %b, 1
  %sameSize = icmp eq i64 %na, %nb
  br i1 %sameSize, label %bytes, label %different
bytes:
  %pa = extractvalue %text %a, 0
  %pb = extractvalue %text %b, 0
  %cmp = call i32 @memcmp(ptr %pa, ptr %pb, i64 %na)
  %same = icmp eq i32 %cmp, 0
  ret i1 %same
different:
  ret i1 false
}
define internal void @tz.printInt(i64 %n) sanitize_address {
  %digits = alloca [20 x i8]
  %negative = icmp slt i64 %n, 0
  %abs = sub i64 0, %n
  %magnitude = select i1 %negative, i64 %abs, i64 %n
  br label %format
format:
  %part = phi i64 [ %magnitude, %0 ], [ %next, %format ]
  %cursor = phi i64 [ 20, %0 ], [ %pos, %format ]
  %next = udiv i64 %part, 10
  %rem = urem i64 %part, 10
  %digit = trunc i64 %rem to i8
  %ascii = add i8 %digit, 48
  %pos = sub i64 %cursor, 1
  %ptr = getelementptr i8, ptr %digits, i64 %pos
  store i8 %ascii, ptr %ptr
  %done = icmp eq i64 %next, 0
  br i1 %done, label %sign, label %format
sign:
  br i1 %negative, label %minus, label %write
minus:
  call i32 @putchar(i32 45)
  br label %write
write:
  %len = sub i64 20, %pos
  %strPtr = insertvalue %text zeroinitializer, ptr %ptr, 0
  %str = insertvalue %text %strPtr, i64 %len, 1
  call void @tz.write(%text %str, i1 false)
  ret void
}
)ll";
    for (std::string op : {"add", "sub", "mul"}) {
        out << "define internal i64 @tz." << op << "(i64 %a, i64 %b, %text %message) sanitize_address {\n"
            << "  %pair = call {i64, i1} @llvm.s" << op << ".with.overflow.i64(i64 %a, i64 %b)\n"
            << "  %bad = extractvalue {i64, i1} %pair, 1\n"
            << "  call void @tz.guard(i1 %bad, %text %message)\n"
            << "  %res = extractvalue {i64, i1} %pair, 0\n  ret i64 %res\n}\n";
    }
    for (std::string op : {"div", "rem"}) {
        out << "define internal i64 @tz." << op << "(i64 %a, i64 %b, %text %zeroMsg, %text %overflowMsg) sanitize_address {\n"
            << "  %zero = icmp eq i64 %b, 0\n  call void @tz.guard(i1 %zero, %text %zeroMsg)\n"
            << "  %min = icmp eq i64 %a, -9223372036854775808\n  %negOne = icmp eq i64 %b, -1\n"
            << "  %bad = and i1 %min, %negOne\n  call void @tz.guard(i1 %bad, %text %overflowMsg)\n"
            << "  %res = s" << op << " i64 %a, %b\n  ret i64 %res\n}\n";
    }
}

} // namespace veloz::native
