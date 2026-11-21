#pragma once
#include <ostream>
#include "veloz.hpp"

namespace veloz::native {

// Array values are (ptr, length). Only heap-owner slots use tz.drop. A region has a
// lazily allocated 4 KiB base chunk and a stack of overflow chunks. Marks restore
// the current bump offset and release all overflow chunks created in the scope.
inline void writeArrayRuntime(std::ostream& out) {
    out << R"ll(
%array = type { ptr, i64 }
%chunk = type { ptr, i64, i64 }
%mark = type { ptr, i64 }
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)

define internal i64 @tz.arrayBytes(i64 %count, %text %sizeMsg, %text %allocMsg) sanitize_address {
  %negative = icmp slt i64 %count, 0
  call void @tz.guard(i1 %negative, %text %sizeMsg)
)ll";
    out << "  %tooLarge = icmp ugt i64 %count, " << maxArrayLength << "\n";
    out << R"ll(  call void @tz.guard(i1 %tooLarge, %text %allocMsg)
  %bytes = mul i64 %count, 8
  ret i64 %bytes
}
define internal ptr @tz.malloc(i64 %bytes, %text %msg) sanitize_address {
  %p = call ptr @malloc(i64 %bytes)
  %failed = icmp eq ptr %p, null
  call void @tz.guard(i1 %failed, %text %msg)
  ret ptr %p
}
define internal void @tz.drop(ptr %slot) sanitize_address {
  %p = load ptr, ptr %slot
  call void @free(ptr %p)
  store %array zeroinitializer, ptr %slot
  ret void
}
define internal ptr @tz.chunk(ptr %previous, i64 %capacity, %text %msg) sanitize_address {
  %bytes = add i64 %capacity, 24
  %p = call ptr @tz.malloc(i64 %bytes, %text %msg)
  store ptr %previous, ptr %p
  %limit = getelementptr %chunk, ptr %p, i32 0, i32 1
  %used = getelementptr %chunk, ptr %p, i32 0, i32 2
  store i64 %capacity, ptr %limit
  store i64 0, ptr %used
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
define internal ptr @tz.bump(ptr %arena, i64 %bytes, %text %msg) sanitize_address {
entry:
  %existing = load ptr, ptr %arena
  %empty = icmp eq ptr %existing, null
  br i1 %empty, label %first, label %capacity
first:
  %base = call ptr @tz.chunk(ptr null, i64 4096, %text %msg)
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
  %small = icmp ult i64 %bytes, 4096
  %space = select i1 %small, i64 4096, i64 %bytes
  %extra = call ptr @tz.chunk(ptr %p, i64 %space, %text %msg)
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
  %keep = or i1 %atGoal, %atBase
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
  call void @free(ptr %base)
  ret void
}
define internal %array @tz.repeat(i64 %value, i64 %count, ptr %arena, %text %sizeMsg, %text %allocMsg) sanitize_address {
entry:
  %n = call i64 @tz.arrayBytes(i64 %count, %text %sizeMsg, %text %allocMsg)
  %empty = icmp eq i64 %count, 0
  br i1 %empty, label %zero, label %allocate
zero:
  ret %array zeroinitializer
allocate:
  %isHeap = icmp eq ptr %arena, null
  br i1 %isHeap, label %heap, label %region
heap:
  %h = call ptr @tz.malloc(i64 %n, %text %allocMsg)
  br label %begin
region:
  %r = call ptr @tz.bump(ptr %arena, i64 %n, %text %allocMsg)
  br label %begin
begin:
  %p = phi ptr [ %h, %heap ], [ %r, %region ]
  br label %fill
fill:
  %i = phi i64 [ 0, %begin ], [ %next, %fill ]
  %dest = getelementptr i64, ptr %p, i64 %i
  store i64 %value, ptr %dest
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, %count
  br i1 %done, label %result, label %fill
result:
  %a = insertvalue %array zeroinitializer, ptr %p, 0
  %array = insertvalue %array %a, i64 %count, 1
  ret %array %array
}
define internal %array @tz.clone(%array %src, %text %msg) sanitize_address {
entry:
  %n = extractvalue %array %src, 1
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %zero, label %copy
zero:
  ret %array zeroinitializer
copy:
  %p = extractvalue %array %src, 0
  %size = mul i64 %n, 8
  %dest = call ptr @tz.malloc(i64 %size, %text %msg)
  call void @llvm.memcpy.p0.p0.i64(ptr %dest, ptr %p, i64 %size, i1 false)
  %a = insertvalue %array %src, ptr %dest, 0
  ret %array %a
}
define internal ptr @tz.at(%array %a, i64 %index, %text %msg) sanitize_address {
  %n = extractvalue %array %a, 1
  %bad = icmp uge i64 %index, %n
  call void @tz.guard(i1 %bad, %text %msg)
  %p = extractvalue %array %a, 0
  %ptr = getelementptr i64, ptr %p, i64 %index
  ret ptr %ptr
}
)ll";
}
} // namespace veloz::native
