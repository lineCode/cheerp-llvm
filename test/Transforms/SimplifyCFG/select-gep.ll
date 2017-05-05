; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -S -simplifycfg < %s | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%ST = type { i8, i8 }

define i8* @test1(%ST* %x, i8* %y) nounwind {
; CHECK-LABEL: @test1(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[CMP:%.*]] = icmp eq %ST* [[X:%.*]], null
; CHECK-NEXT:    [[INCDEC_PTR:%.*]] = getelementptr [[ST:%.*]], %ST* [[X]], i32 0, i32 1
; CHECK-NEXT:    [[INCDEC_PTR_Y:%.*]] = select i1 [[CMP]], i8* [[INCDEC_PTR]], i8* [[Y:%.*]]
; CHECK-NEXT:    ret i8* [[INCDEC_PTR_Y]]
;
entry:
  %cmp = icmp eq %ST* %x, null
  br i1 %cmp, label %if.then, label %if.end

if.then:
  %incdec.ptr = getelementptr %ST, %ST* %x, i32 0, i32 1
  br label %if.end

if.end:
  %x.addr = phi i8* [ %incdec.ptr, %if.then ], [ %y, %entry ]
  ret i8* %x.addr

}
