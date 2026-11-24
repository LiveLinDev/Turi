#pragma once

#include <ostream>
#include "veloz.hpp"

namespace veloz::native {

inline void writeStringRuntime(std::ostream& out) {
    out << R"ll(
define internal void @tz.dropText(ptr %slot) sanitize_address {
entry:
  %hdr = load i32, ptr %slot
  %owned = icmp slt i32 %hdr, 0
  br i1 %owned, label %doFree, label %done
doFree:
  %pf = getelementptr inbounds %text, ptr %slot, i32 0, i32 2
  %p = load ptr, ptr %pf
  call void @free(ptr %p)
  store %text zeroinitializer, ptr %slot
  ret void
done:
  ret void
}
define internal %text @tz.packInline(ptr %src, i64 %len) sanitize_address {
entry:
  %tmp = alloca %text
  store %text zeroinitializer, ptr %tmp
  %len32 = trunc i64 %len to i32
  store i32 %len32, ptr %tmp
  %hasBytes = icmp ugt i64 %len, 0
  br i1 %hasBytes, label %copy, label %done
copy:
  %dst = getelementptr inbounds %text, ptr %tmp, i32 0, i32 1
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 %len, i1 false)
  br label %done
done:
  %res = load %text, ptr %tmp
  ret %text %res
}
define internal %text @tz.packHeap(ptr %p, i64 %len) sanitize_address {
  %pfx = load i32, ptr %p, align 1
  %len32 = trunc i64 %len to i32
  %hdr = or i32 %len32, -2147483648
  %t0 = insertvalue %text zeroinitializer, i32 %hdr, 0
  %t1 = insertvalue %text %t0, i32 %pfx, 1
  %t2 = insertvalue %text %t1, ptr %p, 2
  ret %text %t2
}
define internal %text @tz.packStatic(ptr %p, i64 %len) sanitize_address {
  %pfx = load i32, ptr %p, align 1
  %len32 = trunc i64 %len to i32
  %t0 = insertvalue %text zeroinitializer, i32 %len32, 0
  %t1 = insertvalue %text %t0, i32 %pfx, 1
  %t2 = insertvalue %text %t1, ptr %p, 2
  ret %text %t2
}
define internal %text @tz.cloneText(%text %src, %diag %allocMsg) sanitize_address {
entry:
  %hdr = extractvalue %text %src, 0
  %owned = icmp slt i32 %hdr, 0
  br i1 %owned, label %doCopy, label %trivial
trivial:
  ret %text %src
doCopy:
  %len32 = and i32 %hdr, 2147483647
  %n = zext i32 %len32 to i64
  %p = extractvalue %text %src, 2
  %dst = call ptr @tz.malloc(i64 %n, %diag %allocMsg)
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %p, i64 %n, i1 false)
  %res = call %text @tz.packHeap(ptr %dst, i64 %n)
  ret %text %res
}
define internal %text @tz.textFromSlice(ptr %p, i64 %len, %diag %allocMsg) sanitize_address {
entry:
  %empty = icmp eq i64 %len, 0
  br i1 %empty, label %zero, label %chkInline
zero:
  ret %text zeroinitializer
chkInline:
  %in = icmp ule i64 %len, 12
  br i1 %in, label %packIn, label %packOut
packIn:
  %inRes = call %text @tz.packInline(ptr %p, i64 %len)
  ret %text %inRes
packOut:
)ll";
    out << "  %tooLarge = icmp ugt i64 %len, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %stRes = call %text @tz.packStatic(ptr %p, i64 %len)
  ret %text %stRes
}
define internal %text @tz.textFromOwned(ptr %p, i64 %len, %diag %allocMsg) sanitize_address {
entry:
  %empty = icmp eq i64 %len, 0
  br i1 %empty, label %zero, label %chkInline
zero:
  call void @free(ptr %p)
  ret %text zeroinitializer
chkInline:
  %in = icmp ule i64 %len, 12
  br i1 %in, label %packIn, label %packOut
packIn:
  %inRes = call %text @tz.packInline(ptr %p, i64 %len)
  call void @free(ptr %p)
  ret %text %inRes
packOut:
)ll";
    out << "  %tooLarge = icmp ugt i64 %len, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %hpRes = call %text @tz.packHeap(ptr %p, i64 %len)
  ret %text %hpRes
}
define internal %text @tz.textCopySlice(ptr %p, i64 %len, %diag %allocMsg) sanitize_address {
entry:
  %z = icmp eq i64 %len, 0
  br i1 %z, label %retZ, label %chkIn
retZ:
  ret %text zeroinitializer
chkIn:
  %isIn = icmp ule i64 %len, 12
  br i1 %isIn, label %doIn, label %doHeap
doIn:
  %inRes = call %text @tz.packInline(ptr %p, i64 %len)
  ret %text %inRes
doHeap:
)ll";
    out << "  %tooLarge = icmp ugt i64 %len, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %buf = call ptr @tz.malloc(i64 %len, %diag %allocMsg)
  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %buf, ptr align 1 %p, i64 %len, i1 false)
  %hpRes = call %text @tz.packHeap(ptr %buf, i64 %len)
  ret %text %hpRes
}
define internal i64 @tz.cmpText(%text %a, %text %b) sanitize_address {
entry:
  %sa = alloca %text
  %sb = alloca %text
  store %text %a, ptr %sa
  store %text %b, ptr %sb
  %ha = extractvalue %text %a, 0
  %hb = extractvalue %text %b, 0
  %la32 = and i32 %ha, 2147483647
  %lb32 = and i32 %hb, 2147483647
  %la = zext i32 %la32 to i64
  %lb = zext i32 %lb32 to i64
  %aLtB = icmp ult i64 %la, %lb
  %minL = select i1 %aLtB, i64 %la, i64 %lb
  %has4 = icmp uge i64 %minL, 4
  br i1 %has4, label %fastPfx, label %fullCmp
fastPfx:
  %pa = extractvalue %text %a, 1
  %pb = extractvalue %text %b, 1
  %eqPfx = icmp eq i32 %pa, %pb
  br i1 %eqPfx, label %fullCmp, label %pfxDiff
pfxDiff:
  %ba = call i32 @llvm.bswap.i32(i32 %pa)
  %bb = call i32 @llvm.bswap.i32(i32 %pb)
  %baLt = icmp ult i32 %ba, %bb
  %rDiff = select i1 %baLt, i64 -1, i64 1
  ret i64 %rDiff
fullCmp:
  %zeroMin = icmp eq i64 %minL, 0
  br i1 %zeroMin, label %lenTie, label %doMem
doMem:
  %qa = call ptr @tz.textPtr(ptr %sa)
  %qb = call ptr @tz.textPtr(ptr %sb)
  %mc = call i32 @memcmp(ptr %qa, ptr %qb, i64 %minL)
  %eqMem = icmp eq i32 %mc, 0
  br i1 %eqMem, label %lenTie, label %memDiff
memDiff:
  %mcLt = icmp slt i32 %mc, 0
  %rMem = select i1 %mcLt, i64 -1, i64 1
  ret i64 %rMem
lenTie:
  %sameLen = icmp eq i64 %la, %lb
  br i1 %sameLen, label %retZero, label %diffLen
retZero:
  ret i64 0
diffLen:
  %lenLt = icmp ult i64 %la, %lb
  %rLen = select i1 %lenLt, i64 -1, i64 1
  ret i64 %rLen
}
define internal i64 @tz.byteAt(%text %s, i64 %index, %diag %boundsMsg) sanitize_address {
entry:
  %slot = alloca %text
  store %text %s, ptr %slot
  %hdr = extractvalue %text %s, 0
  %len32 = and i32 %hdr, 2147483647
  %len = zext i32 %len32 to i64
  %bad = icmp uge i64 %index, %len
  call void @tz.guard(i1 %bad, %diag %boundsMsg)
  %p = call ptr @tz.textPtr(ptr %slot)
  %ep = getelementptr inbounds i8, ptr %p, i64 %index
  %b = load i8, ptr %ep
  %res = zext i8 %b to i64
  ret i64 %res
}
define internal %text @tz.sliceText(%text %s, i64 %lo, i64 %hi, %diag %boundsMsg, %diag %allocMsg) sanitize_address {
entry:
  %slot = alloca %text
  store %text %s, ptr %slot
  %hdr = extractvalue %text %s, 0
  %len32 = and i32 %hdr, 2147483647
  %len = zext i32 %len32 to i64
  call void @tz.sliceBounds(i64 %len, i64 %lo, i64 %hi, %diag %boundsMsg)
  %cnt = sub i64 %hi, %lo
  %empty = icmp eq i64 %cnt, 0
  br i1 %empty, label %zero, label %nonEmpty
zero:
  ret %text zeroinitializer
nonEmpty:
  %p = call ptr @tz.textPtr(ptr %slot)
  %sp = getelementptr inbounds i8, ptr %p, i64 %lo
  %in = icmp ule i64 %cnt, 12
  br i1 %in, label %packIn, label %chkStatic
packIn:
  %inRes = call %text @tz.packInline(ptr %sp, i64 %cnt)
  ret %text %inRes
chkStatic:
  %owned = icmp slt i32 %hdr, 0
  br i1 %owned, label %dupHeap, label %staticView
staticView:
  %stRes = call %text @tz.packStatic(ptr %sp, i64 %cnt)
  ret %text %stRes
dupHeap:
  %dst = call ptr @tz.malloc(i64 %cnt, %diag %allocMsg)
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %sp, i64 %cnt, i1 false)
  %hpRes = call %text @tz.packHeap(ptr %dst, i64 %cnt)
  ret %text %hpRes
}
define internal %text @tz.concatText(%text %a, %text %b, %diag %allocMsg) sanitize_address {
entry:
  %sa = alloca %text
  %sb = alloca %text
  %outT = alloca %text
  store %text %a, ptr %sa
  store %text %b, ptr %sb
  %ha = extractvalue %text %a, 0
  %hb = extractvalue %text %b, 0
  %la32 = and i32 %ha, 2147483647
  %lb32 = and i32 %hb, 2147483647
  %la = zext i32 %la32 to i64
  %lb = zext i32 %lb32 to i64
  %total = add i64 %la, %lb
)ll";
    out << "  %tooLarge = icmp ugt i64 %total, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %empty = icmp eq i64 %total, 0
  br i1 %empty, label %zero, label %nonEmpty
zero:
  ret %text zeroinitializer
nonEmpty:
  %qa = call ptr @tz.textPtr(ptr %sa)
  %qb = call ptr @tz.textPtr(ptr %sb)
  %isShort = icmp ule i64 %total, 12
  br i1 %isShort, label %packIn, label %allocHeap
packIn:
  store %text zeroinitializer, ptr %outT
  %t32 = trunc i64 %total to i32
  store i32 %t32, ptr %outT
  %dst0 = getelementptr inbounds %text, ptr %outT, i32 0, i32 1
  %hasA = icmp ugt i64 %la, 0
  br i1 %hasA, label %inCopyA, label %inCopyB
inCopyA:
  call void @llvm.memcpy.p0.p0.i64(ptr %dst0, ptr %qa, i64 %la, i1 false)
  br label %inCopyB
inCopyB:
  %hasB = icmp ugt i64 %lb, 0
  br i1 %hasB, label %inDoB, label %inRet
inDoB:
  %dst1 = getelementptr inbounds i8, ptr %dst0, i64 %la
  call void @llvm.memcpy.p0.p0.i64(ptr %dst1, ptr %qb, i64 %lb, i1 false)
  br label %inRet
inRet:
  %inVal = load %text, ptr %outT
  ret %text %inVal
allocHeap:
  %hp = call ptr @tz.malloc(i64 %total, %diag %allocMsg)
  %hHasA = icmp ugt i64 %la, 0
  br i1 %hHasA, label %hpCopyA, label %hpCopyB
hpCopyA:
  call void @llvm.memcpy.p0.p0.i64(ptr %hp, ptr %qa, i64 %la, i1 false)
  br label %hpCopyB
hpCopyB:
  %hHasB = icmp ugt i64 %lb, 0
  br i1 %hHasB, label %hpDoB, label %hpRet
hpDoB:
  %hp1 = getelementptr inbounds i8, ptr %hp, i64 %la
  call void @llvm.memcpy.p0.p0.i64(ptr %hp1, ptr %qb, i64 %lb, i1 false)
  br label %hpRet
hpRet:
  %hpVal = call %text @tz.packHeap(ptr %hp, i64 %total)
  ret %text %hpVal
}
define internal %text @tz.intToText(i64 %n, %diag %allocMsg) sanitize_address {
entry:
  %digits = alloca [24 x i8]
  %negative = icmp slt i64 %n, 0
  %abs = sub i64 0, %n
  %magnitude = select i1 %negative, i64 %abs, i64 %n
  br label %format
format:
  %part = phi i64 [ %magnitude, %entry ], [ %next, %format ]
  %cursor = phi i64 [ 24, %entry ], [ %pos, %format ]
  %next = udiv i64 %part, 10
  %rem = urem i64 %part, 10
  %digit = trunc i64 %rem to i8
  %ascii = add i8 %digit, 48
  %pos = sub i64 %cursor, 1
  %p = getelementptr i8, ptr %digits, i64 %pos
  store i8 %ascii, ptr %p
  %done = icmp eq i64 %next, 0
  br i1 %done, label %sign, label %format
sign:
  br i1 %negative, label %minus, label %finish
minus:
  %signPos = sub i64 %pos, 1
  %sp = getelementptr i8, ptr %digits, i64 %signPos
  store i8 45, ptr %sp
  br label %finish
finish:
  %startIdx = phi i64 [ %pos, %sign ], [ %signPos, %minus ]
  %len = sub i64 24, %startIdx
  %startPtr = getelementptr i8, ptr %digits, i64 %startIdx
  %isShort = icmp ule i64 %len, 12
  br i1 %isShort, label %retShort, label %retLong
retShort:
  %inRes = call %text @tz.packInline(ptr %startPtr, i64 %len)
  ret %text %inRes
retLong:
  %hp = call ptr @tz.malloc(i64 %len, %diag %allocMsg)
  call void @llvm.memcpy.p0.p0.i64(ptr %hp, ptr %startPtr, i64 %len, i1 false)
  %hpRes = call %text @tz.packHeap(ptr %hp, i64 %len)
  ret %text %hpRes
}
define internal %text @tz.byteToText(i64 %b) sanitize_address {
  %m = and i64 %b, 255
  %pfx = trunc i64 %m to i32
  %t0 = insertvalue %text zeroinitializer, i32 1, 0
  %t1 = insertvalue %text %t0, i32 %pfx, 1
  ret %text %t1
}
define internal i64 @tz.parseIntOpt(%text %s, ptr %okOut) sanitize_address {
entry:
  store i1 false, ptr %okOut
  %slot = alloca %text
  store %text %s, ptr %slot
  %hdr = extractvalue %text %s, 0
  %len32 = and i32 %hdr, 2147483647
  %len = zext i32 %len32 to i64
  %empty = icmp eq i64 %len, 0
  br i1 %empty, label %fail, label %chkSign
chkSign:
  %p = call ptr @tz.textPtr(ptr %slot)
  %c0 = load i8, ptr %p
  %isMinus = icmp eq i8 %c0, 45
  %isPlus = icmp eq i8 %c0, 43
  %hasSign = or i1 %isMinus, %isPlus
  %start = select i1 %hasSign, i64 1, i64 0
  %noDigits = icmp eq i64 %start, %len
  br i1 %noDigits, label %fail, label %prepLoop
prepLoop:
  %limit = select i1 %isMinus, i64 -9223372036854775808, i64 9223372036854775807
  br label %loop
loop:
  %i = phi i64 [ %start, %prepLoop ], [ %nextI, %step ]
  %acc = phi i64 [ 0, %prepLoop ], [ %nextAcc, %step ]
  %cp = getelementptr inbounds i8, ptr %p, i64 %i
  %ch = load i8, ptr %cp
  %d8 = sub i8 %ch, 48
  %badDigit = icmp ugt i8 %d8, 9
  br i1 %badDigit, label %fail, label %chkOv
chkOv:
  %d = zext i8 %d8 to i64
  %rem = sub i64 %limit, %d
  %maxAcc = udiv i64 %rem, 10
  %ov = icmp ugt i64 %acc, %maxAcc
  br i1 %ov, label %fail, label %step
step:
  %t10 = mul i64 %acc, 10
  %nextAcc = add i64 %t10, %d
  %nextI = add i64 %i, 1
  %done = icmp eq i64 %nextI, %len
  br i1 %done, label %ok, label %loop
ok:
  %negVal = sub i64 0, %nextAcc
  %res = select i1 %isMinus, i64 %negVal, i64 %nextAcc
  store i1 true, ptr %okOut
  ret i64 %res
fail:
  ret i64 0
}
define internal i64 @tz.parseInt(%text %s, %diag %parseMsg) sanitize_address {
  %okOut = alloca i1
  %v = call i64 @tz.parseIntOpt(%text %s, ptr %okOut)
  %ok = load i1, ptr %okOut
  %bad = xor i1 %ok, true
  call void @tz.guard(i1 %bad, %diag %parseMsg)
  ret i64 %v
}
define internal i64 @tz.tryParseInt(%text %s, i64 %fb) sanitize_address {
  %okOut = alloca i1
  %v = call i64 @tz.parseIntOpt(%text %s, ptr %okOut)
  %ok = load i1, ptr %okOut
  %res = select i1 %ok, i64 %v, i64 %fb
  ret i64 %res
}
define internal i1 @tz.strStartsWith(%text %s, %text %pfx) sanitize_address {
entry:
  %sa = alloca %text
  %sb = alloca %text
  store %text %s, ptr %sa
  store %text %pfx, ptr %sb
  %ha = extractvalue %text %s, 0
  %hb = extractvalue %text %pfx, 0
  %la32 = and i32 %ha, 2147483647
  %lb32 = and i32 %hb, 2147483647
  %la = zext i32 %la32 to i64
  %lb = zext i32 %lb32 to i64
  %tooShort = icmp ult i64 %la, %lb
  br i1 %tooShort, label %retFalse, label %chkZero
chkZero:
  %z = icmp eq i64 %lb, 0
  br i1 %z, label %retTrue, label %doCmp
doCmp:
  %pa = call ptr @tz.textPtr(ptr %sa)
  %pb = call ptr @tz.textPtr(ptr %sb)
  %mc = call i32 @memcmp(ptr %pa, ptr %pb, i64 %lb)
  %ok = icmp eq i32 %mc, 0
  ret i1 %ok
retTrue:
  ret i1 true
retFalse:
  ret i1 false
}
define internal i1 @tz.strEndsWith(%text %s, %text %sfx) sanitize_address {
entry:
  %sa = alloca %text
  %sb = alloca %text
  store %text %s, ptr %sa
  store %text %sfx, ptr %sb
  %ha = extractvalue %text %s, 0
  %hb = extractvalue %text %sfx, 0
  %la32 = and i32 %ha, 2147483647
  %lb32 = and i32 %hb, 2147483647
  %la = zext i32 %la32 to i64
  %lb = zext i32 %lb32 to i64
  %tooShort = icmp ult i64 %la, %lb
  br i1 %tooShort, label %retFalse, label %chkZero
chkZero:
  %z = icmp eq i64 %lb, 0
  br i1 %z, label %retTrue, label %doCmp
doCmp:
  %pa = call ptr @tz.textPtr(ptr %sa)
  %pb = call ptr @tz.textPtr(ptr %sb)
  %off = sub i64 %la, %lb
  %paOff = getelementptr inbounds i8, ptr %pa, i64 %off
  %mc = call i32 @memcmp(ptr %paOff, ptr %pb, i64 %lb)
  %ok = icmp eq i32 %mc, 0
  ret i1 %ok
retTrue:
  ret i1 true
retFalse:
  ret i1 false
}
define internal i64 @tz.strIndexOf(%text %s, %text %sub) sanitize_address {
entry:
  %sa = alloca %text
  %sb = alloca %text
  store %text %s, ptr %sa
  store %text %sub, ptr %sb
  %ha = extractvalue %text %s, 0
  %hb = extractvalue %text %sub, 0
  %la32 = and i32 %ha, 2147483647
  %lb32 = and i32 %hb, 2147483647
  %la = zext i32 %la32 to i64
  %lb = zext i32 %lb32 to i64
  %z = icmp eq i64 %lb, 0
  br i1 %z, label %retZero, label %chkLen
retZero:
  ret i64 0
chkLen:
  %tooShort = icmp ult i64 %la, %lb
  br i1 %tooShort, label %notFound, label %search
search:
  %pa = call ptr @tz.textPtr(ptr %sa)
  %pb = call ptr @tz.textPtr(ptr %sb)
  %limit = sub i64 %la, %lb
  br label %loop
loop:
  %i = phi i64 [ 0, %search ], [ %next, %nextIter ]
  %cur = getelementptr inbounds i8, ptr %pa, i64 %i
  %mc = call i32 @memcmp(ptr %cur, ptr %pb, i64 %lb)
  %found = icmp eq i32 %mc, 0
  br i1 %found, label %hit, label %nextIter
hit:
  ret i64 %i
nextIter:
  %next = add i64 %i, 1
  %more = icmp ule i64 %next, %limit
  br i1 %more, label %loop, label %notFound
notFound:
  ret i64 -1
}
define internal i1 @tz.strContains(%text %s, %text %sub) sanitize_address {
  %idx = call i64 @tz.strIndexOf(%text %s, %text %sub)
  %found = icmp sge i64 %idx, 0
  ret i1 %found
}
define internal %array @tz.sbWithCap(i64 %cap, %diag %sizeMsg, %diag %allocMsg) sanitize_address {
entry:
  %neg = icmp slt i64 %cap, 0
  call void @tz.guard(i1 %neg, %diag %sizeMsg)
)ll";
    out << "  %tooLarge = icmp ugt i64 %cap, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %empty = icmp eq i64 %cap, 0
  br i1 %empty, label %zero, label %alloc
zero:
  ret %array zeroinitializer
alloc:
  %p = call ptr @tz.malloc(i64 %cap, %diag %allocMsg)
  %c32 = trunc i64 %cap to i32
  %a0 = insertvalue %array zeroinitializer, ptr %p, 0
  %a1 = insertvalue %array %a0, i32 0, 1
  %a2 = insertvalue %array %a1, i32 %c32, 2
  ret %array %a2
}
define internal void @tz.sbGrow(ptr %slot, i64 %minCap, %diag %allocMsg) sanitize_address {
entry:
)ll";
    out << "  %tooLarge = icmp ugt i64 %minCap, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %tooLarge, %diag %allocMsg)
  %arr = load %array, ptr %slot
  %cap32 = extractvalue %array %arr, 2
  %cap = zext i32 %cap32 to i64
  %enough = icmp uge i64 %cap, %minCap
  br i1 %enough, label %done, label %grow
grow:
  %initCap = icmp eq i64 %cap, 0
  %startCap = select i1 %initCap, i64 16, i64 %cap
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
    out << "  %over = icmp ugt i64 %cand, " << maxStringLength << "\n"
        << "  %newCap = select i1 %over, i64 " << maxStringLength << ", i64 %cand\n"
        << R"ll(  %newPtr = call ptr @tz.malloc(i64 %newCap, %diag %allocMsg)
  %oldPtr = extractvalue %array %arr, 0
  %len32 = extractvalue %array %arr, 1
  %len = zext i32 %len32 to i64
  %hasCopy = icmp ugt i64 %len, 0
  br i1 %hasCopy, label %doCopy, label %skipCopy
doCopy:
  call void @llvm.memcpy.p0.p0.i64(ptr %newPtr, ptr %oldPtr, i64 %len, i1 false)
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
define internal void @tz.sbPushRaw(ptr %slot, ptr %p, i64 %n, %diag %allocMsg) sanitize_address {
entry:
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %done, label %push
push:
  %arr = load %array, ptr %slot
  %len32 = extractvalue %array %arr, 1
  %len = zext i32 %len32 to i64
)ll";
    out << "  %tooLongAdd = icmp ugt i64 %n, " << maxStringLength << "\n"
        << "  call void @tz.guard(i1 %tooLongAdd, %diag %allocMsg)\n"
        << "  %maxLeft = sub i64 " << maxStringLength << ", %len\n"
        << R"ll(  %over = icmp ugt i64 %n, %maxLeft
  call void @tz.guard(i1 %over, %diag %allocMsg)
  %need = add i64 %len, %n
  call void @tz.sbGrow(ptr %slot, i64 %need, %diag %allocMsg)
  %pf = getelementptr inbounds %array, ptr %slot, i32 0, i32 0
  %buf = load ptr, ptr %pf
  %dst = getelementptr inbounds i8, ptr %buf, i64 %len
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %p, i64 %n, i1 false)
  %lf = getelementptr inbounds %array, ptr %slot, i32 0, i32 1
  %newLen32 = trunc i64 %need to i32
  store i32 %newLen32, ptr %lf
  br label %done
done:
  ret void
}
define internal void @tz.sbPush(ptr %slot, %text %s, %diag %allocMsg) sanitize_address {
  %ts = alloca %text
  store %text %s, ptr %ts
  %hdr = extractvalue %text %s, 0
  %len32 = and i32 %hdr, 2147483647
  %n = zext i32 %len32 to i64
  %p = call ptr @tz.textPtr(ptr %ts)
  call void @tz.sbPushRaw(ptr %slot, ptr %p, i64 %n, %diag %allocMsg)
  ret void
}
define internal void @tz.sbPushByte(ptr %slot, i64 %b, %diag %allocMsg) sanitize_address {
entry:
  %arr = load %array, ptr %slot
  %len32 = extractvalue %array %arr, 1
  %len = zext i32 %len32 to i64
)ll";
    out << "  %over = icmp uge i64 %len, " << maxStringLength << "\n"
        << R"ll(  call void @tz.guard(i1 %over, %diag %allocMsg)
  %need = add i64 %len, 1
  call void @tz.sbGrow(ptr %slot, i64 %need, %diag %allocMsg)
  %pf = getelementptr inbounds %array, ptr %slot, i32 0, i32 0
  %buf = load ptr, ptr %pf
  %dst = getelementptr inbounds i8, ptr %buf, i64 %len
  %byte = trunc i64 %b to i8
  store i8 %byte, ptr %dst
  %lf = getelementptr inbounds %array, ptr %slot, i32 0, i32 1
  %newLen32 = trunc i64 %need to i32
  store i32 %newLen32, ptr %lf
  ret void
}
define internal void @tz.sbPushInt(ptr %slot, i64 %n, %diag %allocMsg) sanitize_address {
entry:
  %digits = alloca [24 x i8]
  %negative = icmp slt i64 %n, 0
  %abs = sub i64 0, %n
  %magnitude = select i1 %negative, i64 %abs, i64 %n
  br label %format
format:
  %part = phi i64 [ %magnitude, %entry ], [ %next, %format ]
  %cursor = phi i64 [ 24, %entry ], [ %pos, %format ]
  %next = udiv i64 %part, 10
  %rem = urem i64 %part, 10
  %digit = trunc i64 %rem to i8
  %ascii = add i8 %digit, 48
  %pos = sub i64 %cursor, 1
  %p = getelementptr i8, ptr %digits, i64 %pos
  store i8 %ascii, ptr %p
  %done = icmp eq i64 %next, 0
  br i1 %done, label %sign, label %format
sign:
  br i1 %negative, label %minus, label %finish
minus:
  %signPos = sub i64 %pos, 1
  %sp = getelementptr i8, ptr %digits, i64 %signPos
  store i8 45, ptr %sp
  br label %finish
finish:
  %startIdx = phi i64 [ %pos, %sign ], [ %signPos, %minus ]
  %len = sub i64 24, %startIdx
  %startPtr = getelementptr i8, ptr %digits, i64 %startIdx
  call void @tz.sbPushRaw(ptr %slot, ptr %startPtr, i64 %len, %diag %allocMsg)
  ret void
}
define internal %array @tz.sbClone(%array %src, %diag %allocMsg) sanitize_address {
entry:
  %n32 = extractvalue %array %src, 1
  %n = zext i32 %n32 to i64
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %zero, label %copy
zero:
  ret %array zeroinitializer
copy:
  %p = extractvalue %array %src, 0
  %dst = call ptr @tz.malloc(i64 %n, %diag %allocMsg)
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %p, i64 %n, i1 false)
  %a0 = insertvalue %array zeroinitializer, ptr %dst, 0
  %a1 = insertvalue %array %a0, i32 %n32, 1
  %a = insertvalue %array %a1, i32 %n32, 2
  ret %array %a
}
define internal %text @tz.sbStr(%array %sb, %diag %allocMsg) sanitize_address {
entry:
  %n32 = extractvalue %array %sb, 1
  %n = zext i32 %n32 to i64
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %zero, label %chkShort
zero:
  ret %text zeroinitializer
chkShort:
  %p = extractvalue %array %sb, 0
  %isShort = icmp ule i64 %n, 12
  br i1 %isShort, label %retShort, label %retLong
retShort:
  %inRes = call %text @tz.packInline(ptr %p, i64 %n)
  ret %text %inRes
retLong:
  %dst = call ptr @tz.malloc(i64 %n, %diag %allocMsg)
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %p, i64 %n, i1 false)
  %hpRes = call %text @tz.packHeap(ptr %dst, i64 %n)
  ret %text %hpRes
}
define internal %text @tz.sbTake(ptr %slot) sanitize_address {
entry:
  %arr = load %array, ptr %slot
  %n32 = extractvalue %array %arr, 1
  %n = zext i32 %n32 to i64
  %p = extractvalue %array %arr, 0
  %empty = icmp eq i64 %n, 0
  br i1 %empty, label %zero, label %chkShort
zero:
  call void @free(ptr %p)
  store %array zeroinitializer, ptr %slot
  ret %text zeroinitializer
chkShort:
  %isShort = icmp ule i64 %n, 12
  br i1 %isShort, label %retShort, label %retMove
retShort:
  %inRes = call %text @tz.packInline(ptr %p, i64 %n)
  call void @free(ptr %p)
  store %array zeroinitializer, ptr %slot
  ret %text %inRes
retMove:
  %hpRes = call %text @tz.packHeap(ptr %p, i64 %n)
  store %array zeroinitializer, ptr %slot
  ret %text %hpRes
}
)ll";
}

} // namespace veloz::native
