#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace veloz::native {

struct RuntimeDiagnostics {
    std::vector<std::string> ids, files;
    std::string prefix, fileSeparator, startSeparator, endSeparator, suffix, minus;
};

inline void writeRuntime(std::ostream& out, RuntimeABI abi, const RuntimeDiagnostics& diagnostics) {
    out << R"ll(
%text = type { i32, i32, ptr }
%table = type { ptr, i32, i32, i32, i32, i64 }
%dyn = type { ptr, ptr }
%fn = type { ptr, ptr }
%pool = type { ptr, i32, i32, i32, i32 }
%handle = type { i32, i32 }
%diag = type { i32, i32, i64, i64 }
declare i64 @fwrite(ptr, i64, i64, ptr)
declare i32 @putchar(i32)
declare i32 @getchar()
declare i32 @memcmp(ptr, ptr, i64)
declare void @exit(i32) noreturn
declare {i64, i1} @llvm.sadd.with.overflow.i64(i64, i64)
declare {i64, i1} @llvm.ssub.with.overflow.i64(i64, i64)
declare {i64, i1} @llvm.smul.with.overflow.i64(i64, i64)
declare i64 @llvm.smin.i64(i64, i64)
declare i64 @llvm.smax.i64(i64, i64)
declare i32 @llvm.smin.i32(i32, i32)
declare i32 @llvm.smax.i32(i32, i32)
declare i16 @llvm.smin.i16(i16, i16)
declare i16 @llvm.smax.i16(i16, i16)
declare i32 @llvm.bswap.i32(i32)
declare void @llvm.lifetime.start.p0(ptr)
declare void @llvm.lifetime.end.p0(ptr)

)ll";
    const auto table = [&](std::string_view name, const std::vector<std::string>& entries) {
        out << '@' << name << " = private unnamed_addr constant [" << entries.size() << " x %text] [\n";
        for (std::size_t i = 0; i < entries.size(); ++i)
            out << "  " << entries[i] << (i + 1 == entries.size() ? "\n" : ",\n");
        out << "]\n";
    };
    table("tz.diagnosticIds", diagnostics.ids);
    table("tz.diagnosticFiles", diagnostics.files);
    if (abi == RuntimeABI::Windows) {
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
    } else {
        const auto stdoutName = abi == RuntimeABI::Darwin ? "@__stdoutp" : "@stdout";
        const auto stderrName = abi == RuntimeABI::Darwin ? "@__stderrp" : "@stderr";
        out << stdoutName << " = external global ptr\n" << stderrName << " = external global ptr\n"
            << "define internal void @tz.init() sanitize_address { ret void }\n"
            << "define internal ptr @tz.stream(i1 %error) sanitize_address {\n"
            << "  %ref = select i1 %error, ptr " << stderrName << ", ptr " << stdoutName
            << "\n  %stream = load ptr, ptr %ref\n  ret ptr %stream\n}\n";
    }
    out << R"ll(
define internal ptr @tz.textPtr(ptr %slot) sanitize_address {
entry:
  %hdr = load i32, ptr %slot
  %len = and i32 %hdr, 2147483647
  %inline = icmp ule i32 %len, 12
  br i1 %inline, label %in, label %out
in:
  %pin = getelementptr inbounds %text, ptr %slot, i32 0, i32 1
  ret ptr %pin
out:
  %pf = getelementptr inbounds %text, ptr %slot, i32 0, i32 2
  %p = load ptr, ptr %pf
  ret ptr %p
}
define internal void @tz.writeRaw(ptr %p, i64 %n, i1 %error) sanitize_address {
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %done, label %out
out:
  %stream = call ptr @tz.stream(i1 %error)
  call i64 @fwrite(ptr %p, i64 1, i64 %n, ptr %stream)
  br label %done
done:
  ret void
}
define internal void @tz.write(%text %s, i1 %error) sanitize_address {
  %slot = alloca %text
  store %text %s, ptr %slot
  %hdr = extractvalue %text %s, 0
  %len32 = and i32 %hdr, 2147483647
  %n = zext i32 %len32 to i64
  %p = call ptr @tz.textPtr(ptr %slot)
  call void @tz.writeRaw(ptr %p, i64 %n, i1 %error)
  ret void
}
)ll";
    out << "define internal void @tz.abort(%diag %diagnostic) cold noinline noreturn sanitize_address {\n"
        << "  %code = extractvalue %diag %diagnostic, 0\n"
        << "  %file = extractvalue %diag %diagnostic, 1\n"
        << "  %start = extractvalue %diag %diagnostic, 2\n"
        << "  %end = extractvalue %diag %diagnostic, 3\n"
        << "  %idPtr = getelementptr [" << diagnostics.ids.size() << " x %text], ptr @tz.diagnosticIds, i32 0, i32 %code\n"
        << "  %id = load %text, ptr %idPtr\n"
        << "  %filePtr = getelementptr [" << diagnostics.files.size()
        << " x %text], ptr @tz.diagnosticFiles, i32 0, i32 %file\n"
        << "  %fileName = load %text, ptr %filePtr\n"
        << "  call void @tz.write(" << diagnostics.prefix << ", i1 true)\n"
        << "  call void @tz.write(%text %id, i1 true)\n"
        << "  call void @tz.write(" << diagnostics.fileSeparator << ", i1 true)\n"
        << "  call void @tz.write(%text %fileName, i1 true)\n"
        << "  call void @tz.write(" << diagnostics.startSeparator << ", i1 true)\n"
        << "  call void @tz.writeInt(i64 %start, i1 true)\n"
        << "  call void @tz.write(" << diagnostics.endSeparator << ", i1 true)\n"
        << "  call void @tz.writeInt(i64 %end, i1 true)\n"
        << "  call void @tz.write(" << diagnostics.suffix << ", i1 true)\n"
        << R"ll(  call void @exit(i32 1)
  unreachable
}
define internal void @tz.guard(i1 %bad, %diag %diagnostic) alwaysinline sanitize_address {
  br i1 %bad, label %trap, label %ok, !prof !0
trap:
  call void @tz.abort(%diag %diagnostic)
  unreachable
ok:
  ret void
}
!0 = !{!"branch_weights", i32 1, i32 1048575}
define internal i1 @tz.eq(%text %a, %text %b) sanitize_address {
entry:
  %ha = extractvalue %text %a, 0
  %hb = extractvalue %text %b, 0
  %la = and i32 %ha, 2147483647
  %lb = and i32 %hb, 2147483647
  %sameLen = icmp eq i32 %la, %lb
  br i1 %sameLen, label %chkEmpty, label %no
chkEmpty:
  %empty = icmp eq i32 %la, 0
  br i1 %empty, label %yes, label %pfxCmp
pfxCmp:
  %pa = extractvalue %text %a, 1
  %pb = extractvalue %text %b, 1
  %samePfx = icmp eq i32 %pa, %pb
  br i1 %samePfx, label %chkShort, label %no
chkShort:
  %isShort = icmp ule i32 %la, 12
  br i1 %isShort, label %cmpShort, label %cmpLong
cmpShort:
  %ta = extractvalue %text %a, 2
  %tb = extractvalue %text %b, 2
  %sameTail = icmp eq ptr %ta, %tb
  ret i1 %sameTail
cmpLong:
  %qa = extractvalue %text %a, 2
  %qb = extractvalue %text %b, 2
  %samePtr = icmp eq ptr %qa, %qb
  br i1 %samePtr, label %yes, label %doMemcmp
doMemcmp:
  %l64 = zext i32 %la to i64
  %tailLen = sub i64 %l64, 4
  %qa4 = getelementptr inbounds i8, ptr %qa, i64 4
  %qb4 = getelementptr inbounds i8, ptr %qb, i64 4
  %cmp = call i32 @memcmp(ptr %qa4, ptr %qb4, i64 %tailLen)
  %same = icmp eq i32 %cmp, 0
  ret i1 %same
yes:
  ret i1 true
no:
  ret i1 false
}
)ll";
    out << R"ll(define internal void @tz.writeInt(i64 %n, i1 %error) sanitize_address {
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
)ll";
    out << "  call void @tz.write(" << diagnostics.minus << ", i1 %error)\n"
        << R"ll(  br label %write
write:
  %len = sub i64 20, %pos
  call void @tz.writeRaw(ptr %ptr, i64 %len, i1 %error)
  ret void
}
define internal void @tz.printInt(i64 %n) sanitize_address {
  call void @tz.writeInt(i64 %n, i1 false)
  ret void
}
)ll";
    for (std::string op : {"add", "sub", "mul"}) {
        out << "define internal i64 @tz." << op << "(i64 %a, i64 %b, %diag %diagnostic) sanitize_address {\n"
            << "  %pair = call {i64, i1} @llvm.s" << op << ".with.overflow.i64(i64 %a, i64 %b)\n"
            << "  %bad = extractvalue {i64, i1} %pair, 1\n"
            << "  call void @tz.guard(i1 %bad, %diag %diagnostic)\n"
            << "  %res = extractvalue {i64, i1} %pair, 0\n  ret i64 %res\n}\n";
    }
    for (std::string op : {"div", "rem"}) {
        out << "define internal i64 @tz." << op << "(i64 %a, i64 %b, %diag %zeroDiagnostic, %diag %overflowDiagnostic) sanitize_address {\n"
            << "  %zero = icmp eq i64 %b, 0\n  call void @tz.guard(i1 %zero, %diag %zeroDiagnostic)\n"
            << "  %min = icmp eq i64 %a, -9223372036854775808\n  %negOne = icmp eq i64 %b, -1\n"
            << "  %bad = and i1 %min, %negOne\n  call void @tz.guard(i1 %bad, %diag %overflowDiagnostic)\n"
            << "  %res = s" << op << " i64 %a, %b\n  ret i64 %res\n}\n";
    }
}

} // namespace veloz::native
