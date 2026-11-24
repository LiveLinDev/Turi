#pragma once
#include <ostream>
#include "veloz.hpp"

namespace veloz::native {

// Array values are (ptr, length). Only heap-owner slots use tz.drop. A region sizes
// its base from the first reservation (minimum 256 bytes) and uses exact overflow
// chunks. Marks restore the bump offset and release chunks created in the scope.
inline void writeArrayRuntime(std::ostream& out, bool auditCopies) {
    if (auditCopies) {
        out << "declare i64 @turi_audit_copy(i64, i64, i64)\n"
            << "declare void @turi_audit_copied(i64)\n"
            << "declare void @turi_audit_fill(i64, i1)\n";
    }
    out << R"ll(
%array = type { ptr, i32, i32 }
%chunk = type { ptr, i64, i64, i64 }
%mark = type { ptr, i64 }
declare noalias ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)

define internal void @tz.ptrCopy(ptr %dst, ptr %src, i64 %bytes) sanitize_address {
entry:
  %pos = icmp ugt i64 %bytes, 0
  br i1 %pos, label %doCopy, label %done
doCopy:
  call void @llvm.memmove.p0.p0.i64(ptr %dst, ptr %src, i64 %bytes, i1 false)
  br label %done
done:
  ret void
}
define internal void @tz.ptrZero(ptr %dst, i64 %bytes) sanitize_address {
entry:
  %pos = icmp ugt i64 %bytes, 0
  br i1 %pos, label %doZero, label %done
doZero:
  call void @llvm.memset.p0.i64(ptr %dst, i8 0, i64 %bytes, i1 false)
  br label %done
done:
  ret void
}

define internal i64 @tz.arrayBytes(i64 %count, %diag %sizeMsg, %diag %allocMsg) sanitize_address {
  %negative = icmp slt i64 %count, 0
  call void @tz.guard(i1 %negative, %diag %sizeMsg)
)ll";
    out << "  %tooLarge = icmp ugt i64 %count, " << maxArrayLength << "\n";
    out << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %bytes = mul i64 %count, 8
  ret i64 %bytes
}
define internal noalias ptr @tz.malloc(i64 %bytes, %diag %msg) sanitize_address {
  %p = call ptr @malloc(i64 %bytes)
  %failed = icmp eq ptr %p, null
  call void @tz.guard(i1 %failed, %diag %msg)
  ret ptr %p
}
define internal void @tz.drop(ptr %slot) sanitize_address {
  %p = load ptr, ptr %slot
  call void @free(ptr %p)
  store %array zeroinitializer, ptr %slot
  ret void
}
define internal ptr @tz.chunk(ptr %previous, i64 %capacity, %diag %msg) sanitize_address {
  %bytes = add i64 %capacity, 32
  %p = call ptr @tz.malloc(i64 %bytes, %diag %msg)
  store ptr %previous, ptr %p
  %limit = getelementptr %chunk, ptr %p, i32 0, i32 1
  %used = getelementptr %chunk, ptr %p, i32 0, i32 2
  %heapp = getelementptr %chunk, ptr %p, i32 0, i32 3
  store i64 %capacity, ptr %limit
  store i64 0, ptr %used
  store i64 1, ptr %heapp
  ret ptr %p
}
define internal %mark @tz.mark(ptr %arena) sanitize_address {
  %p = load ptr, ptr %arena
  %empty = icmp eq ptr %p, null
  br i1 %empty, label %none, label %active
none:
  ret %mark zeroinitializer
active:
  %used = getelementptr %chunk, ptr %p, i32 0, i32 2
  %n = load i64, ptr %used
  %m = insertvalue %mark zeroinitializer, ptr %p, 0
  %mark = insertvalue %mark %m, i64 %n, 1
  ret %mark %mark
}
define internal noalias ptr @tz.bump(ptr %arena, i64 %bytes, %diag %msg) sanitize_address {
entry:
  %existing = load ptr, ptr %arena
  %empty = icmp eq ptr %existing, null
  br i1 %empty, label %first, label %capacity
first:
  %tiny = icmp ult i64 %bytes, 256
  %firstSpace = select i1 %tiny, i64 256, i64 %bytes
  %base = call ptr @tz.chunk(ptr null, i64 %firstSpace, %diag %msg)
  store ptr %base, ptr %arena
  br label %capacity
capacity:
  %p = phi ptr [ %existing, %entry ], [ %base, %first ]
  %limitp = getelementptr %chunk, ptr %p, i32 0, i32 1
  %usedp = getelementptr %chunk, ptr %p, i32 0, i32 2
  %limit = load i64, ptr %limitp
  %used = load i64, ptr %usedp
  %left = sub i64 %limit, %used
  %fits = icmp ule i64 %bytes, %left
  br i1 %fits, label %found, label %spill
spill:
  %extra = call ptr @tz.chunk(ptr %p, i64 %bytes, %diag %msg)
  store ptr %extra, ptr %arena
  br label %found
found:
  %chunk = phi ptr [ %p, %capacity ], [ %extra, %spill ]
  %offset = phi i64 [ %used, %capacity ], [ 0, %spill ]
  %size = add i64 %offset, %bytes
  %u = getelementptr %chunk, ptr %chunk, i32 0, i32 2
  store i64 %size, ptr %u
  %data = getelementptr %chunk, ptr %chunk, i32 1
  %next = getelementptr i8, ptr %data, i64 %offset
  ret ptr %next
}
define internal void @tz.rewind(ptr %arena, %mark %mark) sanitize_address {
entry:
  %goal = extractvalue %mark %mark, 0
  %offset = extractvalue %mark %mark, 1
  br label %pop
pop:
  %p = load ptr, ptr %arena
  %empty = icmp eq ptr %p, null
  br i1 %empty, label %done, label %check
check:
  %prev = load ptr, ptr %p
  %atGoal = icmp eq ptr %p, %goal
  %atBase = icmp eq ptr %prev, null
  %limitp = getelementptr %chunk, ptr %p, i32 0, i32 1
  %capacity = load i64, ptr %limitp
  %cacheable = icmp ule i64 %capacity, 4096
  %heapp = getelementptr %chunk, ptr %p, i32 0, i32 3
  %isHeap = load i64, ptr %heapp
  %isStack = icmp eq i64 %isHeap, 0
  %keepBase = and i1 %atBase, %cacheable
  %keep1 = or i1 %atGoal, %keepBase
  %keep = or i1 %keep1, %isStack
  br i1 %keep, label %reset, label %free
free:
  store ptr %prev, ptr %arena
  call void @free(ptr %p)
  br label %pop
reset:
  %used = getelementptr %chunk, ptr %p, i32 0, i32 2
  store i64 %offset, ptr %used
  br label %done
done:
  ret void
}
define internal void @tz.finish(ptr %arena) sanitize_address {
  call void @tz.rewind(ptr %arena, %mark zeroinitializer)
  %base = load ptr, ptr %arena
  %empty = icmp eq ptr %base, null
  br i1 %empty, label %done, label %check
check:
  %heapp = getelementptr %chunk, ptr %base, i32 0, i32 3
  %isHeap = load i64, ptr %heapp
  %heap = icmp ne i64 %isHeap, 0
  br i1 %heap, label %drop, label %done
drop:
  call void @free(ptr %base)
  br label %done
done:
  ret void
}
define internal %array @tz.repeat(i64 %value, i64 %count, ptr %arena, i1 %initialize, %diag %sizeMsg, %diag %allocMsg) sanitize_address {
entry:
  %n = call i64 @tz.arrayBytes(i64 %count, %diag %sizeMsg, %diag %allocMsg)
)ll";
    if (auditCopies) out << "  call void @turi_audit_fill(i64 %n, i1 %initialize)\n";
    out << R"ll(  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %zero, label %allocate
zero:
  ret %array zeroinitializer
allocate:
  %isHeap = icmp eq ptr %arena, null
  br i1 %isHeap, label %heap, label %region
heap:
  %h = call ptr @tz.malloc(i64 %n, %diag %allocMsg)
  br label %begin
region:
  %r = call ptr @tz.bump(ptr %arena, i64 %n, %diag %allocMsg)
  br label %begin
begin:
  %p = phi ptr [ %h, %heap ], [ %r, %region ]
  br i1 %initialize, label %checkzero, label %result
checkzero:
  %iszero = icmp eq i64 %value, 0
  br i1 %iszero, label %zerofill, label %fill
zerofill:
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %n, i1 false)
  br label %result
fill:
  %i = phi i64 [ 0, %checkzero ], [ %next, %fill ]
  %dest = getelementptr i64, ptr %p, i64 %i
  store i64 %value, ptr %dest
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, %count
  br i1 %done, label %result, label %fill
result:
  %a0 = insertvalue %array zeroinitializer, ptr %p, 0
  %cnt32 = trunc i64 %count to i32
  %a1 = insertvalue %array %a0, i32 %cnt32, 1
  %array = insertvalue %array %a1, i32 %cnt32, 2
  ret %array %array
}
)ll";
    out << "define internal %array @tz.cloneBytes(%array %src, i64 %elemBytes, ptr %arena, %diag %msg";
    if (auditCopies) out << ", i64 %siteStart, i64 %siteEnd";
    out << R"ll() sanitize_address {
entry:
  %n32 = extractvalue %array %src, 1
  %n = zext i32 %n32 to i64
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %zero, label %copy
zero:
  ret %array zeroinitializer
copy:
  %p = extractvalue %array %src, 0
  %size = mul i64 %n, %elemBytes
)ll";
    if (auditCopies) out << "  %copyTimer = call i64 @turi_audit_copy(i64 %size, i64 %siteStart, i64 %siteEnd)\n";
    out << R"ll(  %isHeap = icmp eq ptr %arena, null
  br i1 %isHeap, label %heap, label %region
heap:
  %h = call ptr @tz.malloc(i64 %size, %diag %msg)
  br label %doCopy
region:
  %r = call ptr @tz.bump(ptr %arena, i64 %size, %diag %msg)
  br label %doCopy
doCopy:
  %dest = phi ptr [ %h, %heap ], [ %r, %region ]
  call void @llvm.memcpy.p0.p0.i64(ptr %dest, ptr %p, i64 %size, i1 false)
)ll";
    if (auditCopies) out << "  call void @turi_audit_copied(i64 %copyTimer)\n";
    out << R"ll(  %a0 = insertvalue %array zeroinitializer, ptr %dest, 0
  %a1 = insertvalue %array %a0, i32 %n32, 1
  %a = insertvalue %array %a1, i32 %n32, 2
  ret %array %a
}
)ll";
    out << "define internal %array @tz.clone(%array %src, ptr %arena, %diag %msg";
    if (auditCopies) out << ", i64 %siteStart, i64 %siteEnd";
    out << ") sanitize_address {\n  %res = call %array @tz.cloneBytes(%array %src, i64 8, ptr %arena, %diag %msg";
    if (auditCopies) out << ", i64 %siteStart, i64 %siteEnd";
    out << ")\n  ret %array %res\n}\n";
    out << R"ll(define internal ptr @tz.at(%array %a, i64 %index, %diag %msg) sanitize_address {
  %n32 = extractvalue %array %a, 1
  %n = zext i32 %n32 to i64
  %bad = icmp uge i64 %index, %n
  call void @tz.guard(i1 %bad, %diag %msg)
  %p = extractvalue %array %a, 0
  %ptr = getelementptr i64, ptr %p, i64 %index
  ret ptr %ptr
}
define internal noalias ptr @tz.allocBuf(i64 %count, i64 %elemBytes, %diag %sizeMsg, %diag %allocMsg) sanitize_address {
entry:
  %negative = icmp slt i64 %count, 0
  call void @tz.guard(i1 %negative, %diag %sizeMsg)
)ll";
    out << "  %tooLarge = icmp ugt i64 %count, " << maxArrayLength << "\n";
    out << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %zero, label %alloc
zero:
  ret ptr null
alloc:
  %rawBytes = mul i64 %count, %elemBytes
  %isZero = icmp eq i64 %rawBytes, 0
  %allocBytes = select i1 %isZero, i64 1, i64 %rawBytes
  %p = call ptr @tz.malloc(i64 %allocBytes, %diag %allocMsg)
  ret ptr %p
}
define internal void @tz.growBuf(ptr %slot, i64 %elemBytes, i64 %minCap, %diag %allocMsg) sanitize_address {
entry:
)ll";
    out << "  %tooLarge = icmp ugt i64 %minCap, " << maxArrayLength << "\n";
    out << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %arr = load %array, ptr %slot
  %cap32 = extractvalue %array %arr, 2
  %cap = zext i32 %cap32 to i64
  %enough = icmp uge i64 %cap, %minCap
  br i1 %enough, label %done, label %grow
grow:
  %initCap = icmp eq i64 %cap, 0
  %startCap = select i1 %initCap, i64 8, i64 %cap
  br label %doubleLoop
doubleLoop:
  %cand = phi i64 [ %startCap, %grow ], [ %doubled, %doubleStep ]
  %okCap = icmp uge i64 %cand, %minCap
  br i1 %okCap, label %capped, label %doubleStep
doubleStep:
  %doubled = mul i64 %cand, 2
  br label %doubleLoop
capped:
)ll";
    out << "  %over = icmp ugt i64 %cand, " << maxArrayLength << "\n"
        << "  %newCap = select i1 %over, i64 " << maxArrayLength << ", i64 %cand\n";
    out << R"ll(  %rawBytes = mul i64 %newCap, %elemBytes
  %zeroB = icmp eq i64 %rawBytes, 0
  %allocBytes = select i1 %zeroB, i64 1, i64 %rawBytes
  %newPtr = call ptr @tz.malloc(i64 %allocBytes, %diag %allocMsg)
  %oldPtr = extractvalue %array %arr, 0
  %len32 = extractvalue %array %arr, 1
  %len = zext i32 %len32 to i64
  %copyBytes = mul i64 %len, %elemBytes
  %hasCopy = icmp ugt i64 %copyBytes, 0
  br i1 %hasCopy, label %doCopy, label %skipCopy
doCopy:
  call void @llvm.memcpy.p0.p0.i64(ptr %newPtr, ptr %oldPtr, i64 %copyBytes, i1 false)
  br label %skipCopy
skipCopy:
  call void @free(ptr %oldPtr)
  %newCap32 = trunc i64 %newCap to i32
  %a0 = insertvalue %array %arr, ptr %newPtr, 0
  %a1 = insertvalue %array %a0, i32 %newCap32, 2
  store %array %a1, ptr %slot
  br label %done
done:
  ret void
}
define internal void @tz.sliceBounds(i64 %len, i64 %lo, i64 %hi, %diag %boundsMsg) sanitize_address {
  %badLo = icmp slt i64 %lo, 0
  %badOrd = icmp slt i64 %hi, %lo
  %badHi = icmp sgt i64 %hi, %len
  %b0 = or i1 %badLo, %badOrd
  %bad = or i1 %b0, %badHi
  call void @tz.guard(i1 %bad, %diag %boundsMsg)
  ret void
}
)ll";
    for (const int bits : {8, 16, 32}) {
        const int elemBytes = bits / 8;
        const std::string w = "i" + std::to_string(bits);
        out << "define internal i64 @tz.arrayBytes." << w
            << "(i64 %count, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\n"
            << "  %negative = icmp slt i64 %count, 0\n"
            << "  call void @tz.guard(i1 %negative, %diag %sizeMsg)\n"
            << "  %tooLarge = icmp ugt i64 %count, " << maxArrayLength << "\n"
            << "  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)\n"
            << "  %bytes = mul i64 %count, " << elemBytes << "\n"
            << "  ret i64 %bytes\n}\n"
            << "define internal %array @tz.repeat." << w
            << "(i64 %value, i64 %count, ptr %arena, i1 %initialize, %diag %sizeMsg, %diag %allocMsg) sanitize_address {\n"
            << "entry:\n"
            << "  %n = call i64 @tz.arrayBytes." << w
            << "(i64 %count, %diag %sizeMsg, %diag %allocMsg)\n"
            << "  %empty = icmp eq i64 %count, 0\n"
            << "  br i1 %empty, label %zero, label %allocate\n"
            << "zero:\n  ret %array zeroinitializer\n"
            << "allocate:\n"
            << "  %isHeap = icmp eq ptr %arena, null\n"
            << "  br i1 %isHeap, label %heap, label %region\n"
            << "heap:\n  %h = call ptr @tz.malloc(i64 %n, %diag %allocMsg)\n  br label %begin\n"
            << "region:\n  %r = call ptr @tz.bump(ptr %arena, i64 %n, %diag %allocMsg)\n  br label %begin\n"
            << "begin:\n  %p = phi ptr [ %h, %heap ], [ %r, %region ]\n"
            << "  br i1 %initialize, label %checkzero, label %result\n"
            << "checkzero:\n  %iszero = icmp eq i64 %value, 0\n"
            << "  br i1 %iszero, label %zerofill, label %fillhead\n"
            << "zerofill:\n  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 %n, i1 false)\n  br label %result\n"
            << "fillhead:\n  %valw = trunc i64 %value to " << w << "\n  br label %fill\n"
            << "fill:\n  %i = phi i64 [ 0, %fillhead ], [ %next, %fill ]\n"
            << "  %dest = getelementptr " << w << ", ptr %p, i64 %i\n"
            << "  store " << w << " %valw, ptr %dest\n"
            << "  %next = add i64 %i, 1\n"
            << "  %done = icmp eq i64 %next, %count\n"
            << "  br i1 %done, label %result, label %fill\n"
            << "result:\n  %a0 = insertvalue %array zeroinitializer, ptr %p, 0\n"
            << "  %cnt32 = trunc i64 %count to i32\n"
            << "  %a1 = insertvalue %array %a0, i32 %cnt32, 1\n"
            << "  %array = insertvalue %array %a1, i32 %cnt32, 2\n"
            << "  ret %array %array\n}\n"
            << "define internal ptr @tz.at." << w
            << "(%array %a, i64 %index, %diag %msg) sanitize_address {\n"
            << "  %n32 = extractvalue %array %a, 1\n"
            << "  %n = zext i32 %n32 to i64\n"
            << "  %bad = icmp uge i64 %index, %n\n"
            << "  call void @tz.guard(i1 %bad, %diag %msg)\n"
            << "  %p = extractvalue %array %a, 0\n"
            << "  %ptr = getelementptr " << w << ", ptr %p, i64 %index\n"
            << "  ret ptr %ptr\n}\n"
            << "define internal %array @tz.clone." << w << "(%array %src, ptr %arena, %diag %msg"
            << (auditCopies ? ", i64 %siteStart, i64 %siteEnd" : "") << ") sanitize_address {\n"
            << "  %res = call %array @tz.cloneBytes(%array %src, i64 " << elemBytes
            << ", ptr %arena, %diag %msg"
            << (auditCopies ? ", i64 %siteStart, i64 %siteEnd" : "") << ")\n"
            << "  ret %array %res\n}\n";
    }
}
} // namespace veloz::native
