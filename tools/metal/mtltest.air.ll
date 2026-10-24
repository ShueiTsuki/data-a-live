; mtltest's shaders as AIR (tools/metal/mtltest.metal compiled by Apple's metal on CI, then
; disassembled and linked by tools/metal/mtltest_air.sh). build_plugin_linux.sh assembles it.
; ModuleID = '/tmp/tmp.6dECocV6rL/linked.bc'
source_filename = "llvm-link"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.0.0"

@__air_sampler_state = internal addrspace(2) constant i64 -9188470239253757879, align 8
@__air_sampler_state.1 = internal addrspace(2) constant i64 -9188470239253755319, align 8
@__air_sampler_state.2 = internal addrspace(2) constant i64 -9188470239253722624, align 8
@__air_sampler_state.3 = internal addrspace(2) constant i64 -9188470239253757879, align 8
@__air_sampler_state.1.4 = internal addrspace(2) constant i64 -9188470239253755319, align 8
@__air_sampler_state.5 = internal addrspace(2) constant i64 -9188470239253757879, align 8

; Function Attrs: norecurse nounwind memory(argmem: readwrite)
define void @fill(ptr addrspace(1) captures(none) "air-buffer-no-alias" %0, i32 %1) local_unnamed_addr #0 {
  %3 = mul i32 %1, 3
  %4 = add i32 %3, 4096
  %5 = zext i32 %1 to i64
  %6 = getelementptr inbounds i32, ptr addrspace(1) %0, i64 %5
  store i32 %4, ptr addrspace(1) %6, align 4, !tbaa !85, !alias.scope !89
  ret void
}

; Function Attrs: nounwind memory(none)
define <{ <4 x float> }> @tri_vs(i32 %0) local_unnamed_addr #1 {
  %2 = alloca [3 x <2 x float>], align 8
  %3 = bitcast ptr %2 to ptr
  call void @llvm.lifetime.start.p0(ptr %2)
  %4 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 0
  store <2 x float> <float -1.000000e+00, float 1.000000e+00>, ptr %4, align 8
  %5 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 1
  store <2 x float> splat (float 1.000000e+00), ptr %5, align 8
  %6 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 2
  store <2 x float> splat (float -1.000000e+00), ptr %6, align 8
  %7 = zext i32 %0 to i64
  %8 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 %7
  %9 = load <2 x float>, ptr %8, align 8, !tbaa !92
  %10 = shufflevector <2 x float> %9, <2 x float> undef, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
  %11 = shufflevector <4 x float> %10, <4 x float> <float undef, float undef, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 1, i32 6, i32 7>
  call void @llvm.lifetime.end.p0(ptr %2)
  %12 = insertvalue <{ <4 x float> }> undef, <4 x float> %11, 0
  ret <{ <4 x float> }> %12
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.start.p0(ptr captures(none)) #2

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare void @llvm.lifetime.end.p0(ptr captures(none)) #2

; Function Attrs: nounwind memory(none)
define <{ <4 x float>, <2 x float> }> @quad_vs(i32 %0) local_unnamed_addr #1 {
  %2 = alloca [3 x <2 x float>], align 8
  %3 = bitcast ptr %2 to ptr
  call void @llvm.lifetime.start.p0(ptr %2)
  %4 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 0
  store <2 x float> splat (float -1.000000e+00), ptr %4, align 8
  %5 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 1
  store <2 x float> <float 3.000000e+00, float -1.000000e+00>, ptr %5, align 8
  %6 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 2
  store <2 x float> <float -1.000000e+00, float 3.000000e+00>, ptr %6, align 8
  %7 = zext i32 %0 to i64
  %8 = getelementptr inbounds [3 x <2 x float>], ptr %2, i64 0, i64 %7
  %9 = load <2 x float>, ptr %8, align 8, !tbaa !92
  %10 = shufflevector <2 x float> %9, <2 x float> undef, <4 x i32> <i32 0, i32 1, i32 poison, i32 poison>
  %11 = shufflevector <4 x float> %10, <4 x float> <float undef, float undef, float 0.000000e+00, float 1.000000e+00>, <4 x i32> <i32 0, i32 1, i32 6, i32 7>
  %12 = extractelement <2 x float> %9, i64 0
  %13 = fadd fast float %12, 1.000000e+00
  %14 = fmul fast float %13, 5.000000e-01
  %15 = insertelement <2 x float> undef, float %14, i64 0
  %16 = extractelement <2 x float> %9, i64 1
  %17 = fsub fast float 1.000000e+00, %16
  %18 = fmul fast float %17, 5.000000e-01
  %19 = insertelement <2 x float> %15, float %18, i64 1
  call void @llvm.lifetime.end.p0(ptr %2)
  %20 = insertvalue <{ <4 x float>, <2 x float> }> undef, <4 x float> %11, 0
  %21 = insertvalue <{ <4 x float>, <2 x float> }> %20, <2 x float> %19, 1
  ret <{ <4 x float>, <2 x float> }> %21
}

; Function Attrs: norecurse nounwind memory(none)
define <4 x float> @tri_fs() local_unnamed_addr #3 {
  ret <4 x float> <float 0.000000e+00, float 1.000000e+00, float 0.000000e+00, float 1.000000e+00>
}

; Function Attrs: convergent nounwind memory(argmem: read)
define <4 x float> @tex_fs(<4 x float> %0, <2 x float> %1, ptr addrspace(1) readonly captures(none) %2) local_unnamed_addr #4 {
  %4 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) @__air_sampler_state, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  %5 = extractvalue { <4 x float>, i8 } %4, 0
  ret <4 x float> %5
}

; Function Attrs: convergent nounwind memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #5

; Function Attrs: convergent nounwind memory(argmem: read)
define <4 x float> @tex_lin_fs(<4 x float> %0, <2 x float> %1, ptr addrspace(1) readonly captures(none) %2) local_unnamed_addr #4 {
  %4 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) @__air_sampler_state.1, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  %5 = extractvalue { <4 x float>, i8 } %4, 0
  ret <4 x float> %5
}

; Function Attrs: convergent nounwind memory(read)
define <4 x float> @tex_px_fs(<4 x float> %0, <2 x float> %1, ptr addrspace(1) %2) local_unnamed_addr #6 {
  %4 = tail call i32 @air.get_width_texture_2d(ptr addrspace(1) readonly captures(none) %2, i32 0) #7, !alias.scope !93
  %5 = tail call fast float @air.convert.f.f32.u.i32(i32 %4) #8
  %6 = insertelement <2 x float> undef, float %5, i64 0
  %7 = tail call i32 @air.get_height_texture_2d(ptr addrspace(1) readonly captures(none) %2, i32 0) #7, !alias.scope !93
  %8 = tail call fast float @air.convert.f.f32.u.i32(i32 %7) #8
  %9 = insertelement <2 x float> %6, float %8, i64 1
  %10 = fmul fast <2 x float> %9, %1
  %11 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) @__air_sampler_state.2, <2 x float> %10, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  %12 = extractvalue { <4 x float>, i8 } %11, 0
  ret <4 x float> %12
}

; Function Attrs: nounwind memory(argmem: read)
declare i32 @air.get_width_texture_2d(ptr addrspace(1) readonly captures(none), i32) local_unnamed_addr #7

; Function Attrs: nounwind memory(none)
declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr #8

; Function Attrs: nounwind memory(argmem: read)
declare i32 @air.get_height_texture_2d(ptr addrspace(1) readonly captures(none), i32) local_unnamed_addr #7

; Function Attrs: convergent nounwind memory(argmem: read)
define <4 x float> @tex3_fs(<4 x float> %0, <2 x float> %1, ptr addrspace(1) readonly captures(none) %2) local_unnamed_addr #4 {
  %4 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) @__air_sampler_state.3, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  %5 = extractvalue { <4 x float>, i8 } %4, 0
  ret <4 x float> %5
}

; Function Attrs: convergent nounwind memory(argmem: read)
define <4 x float> @sweep_fs(<4 x float> %0, <2 x float> %1, ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) %3, ptr addrspace(2) readonly captures(none) dereferenceable(8) "air-buffer-no-alias" %4) local_unnamed_addr #4 {
  %6 = load <2 x float>, ptr addrspace(2) %4, align 8, !tbaa !92, !alias.scope !96, !noalias !99
  %7 = fmul fast <2 x float> %6, %1
  %8 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) %3, <2 x float> %7, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5, !alias.scope !99, !noalias !96
  %9 = extractvalue { <4 x float>, i8 } %8, 0
  ret <4 x float> %9
}

; Function Attrs: norecurse nounwind memory(none)
define <4 x float> @solid_fs(<4 x float> %0, <2 x float> %1) local_unnamed_addr #3 {
  ret <4 x float> <float 2.500000e-01, float 5.000000e-01, float 7.500000e-01, float 1.000000e+00>
}

; Function Attrs: norecurse nounwind memory(none)
define <4 x float> @fetch_fs(<4 x float> %0, <2 x float> %1, <4 x float> %2) local_unnamed_addr #3 {
  %4 = shufflevector <4 x float> %2, <4 x float> undef, <4 x i32> <i32 2, i32 1, i32 0, i32 3>
  ret <4 x float> %4
}

; Function Attrs: convergent nounwind memory(argmem: read)
define <4 x float> @fetch_tex_fs(<4 x float> %0, <2 x float> %1, <4 x float> %2, ptr addrspace(1) readonly captures(none) %3) local_unnamed_addr #4 {
  %5 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %3, ptr addrspace(2) readonly captures(none) @__air_sampler_state.1.4, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  %6 = extractvalue { <4 x float>, i8 } %5, 0
  %7 = fmul fast <4 x float> %6, %2
  ret <4 x float> %7
}

; Function Attrs: norecurse nounwind memory(none)
define <{ <4 x float>, <4 x float> }> @fetch2_fs(<4 x float> %0, <2 x float> %1, <4 x float> %2, <4 x float> %3) local_unnamed_addr #3 {
  %5 = insertvalue <{ <4 x float>, <4 x float> }> undef, <4 x float> %3, 0
  %6 = insertvalue <{ <4 x float>, <4 x float> }> %5, <4 x float> %2, 1
  ret <{ <4 x float>, <4 x float> }> %6
}

; Function Attrs: convergent nounwind memory(read)
define <4 x float> @index_fs(<4 x float> %0, <2 x float> %1, ptr addrspace(2) readonly captures(none) dereferenceable(4) "air-buffer-no-alias" %2, ptr addrspace(1) %3, ptr addrspace(1) %4, ptr addrspace(1) %5, ptr addrspace(1) %6, ptr addrspace(1) %7, ptr addrspace(1) %8, ptr addrspace(1) %9, ptr addrspace(1) %10, ptr addrspace(1) %11, ptr addrspace(1) %12, ptr addrspace(1) %13, ptr addrspace(1) %14, ptr addrspace(1) %15, ptr addrspace(1) %16, ptr addrspace(1) %17, ptr addrspace(1) %18) local_unnamed_addr #6 {
  %20 = load i32, ptr addrspace(2) %2, align 4, !tbaa !85, !alias.scope !102, !noalias !105
  switch i32 %20, label %51 [
    i32 0, label %21
    i32 1, label %23
    i32 2, label %25
    i32 3, label %27
    i32 4, label %29
    i32 5, label %31
    i32 6, label %33
    i32 7, label %35
    i32 8, label %37
    i32 9, label %39
    i32 10, label %41
    i32 11, label %43
    i32 12, label %45
    i32 13, label %47
    i32 14, label %49
  ]

21:                                               ; preds = %19
  %22 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %3, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

23:                                               ; preds = %19
  %24 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %4, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

25:                                               ; preds = %19
  %26 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %5, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

27:                                               ; preds = %19
  %28 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %6, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

29:                                               ; preds = %19
  %30 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %7, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

31:                                               ; preds = %19
  %32 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %8, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

33:                                               ; preds = %19
  %34 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %9, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

35:                                               ; preds = %19
  %36 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %10, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

37:                                               ; preds = %19
  %38 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %11, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

39:                                               ; preds = %19
  %40 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %12, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

41:                                               ; preds = %19
  %42 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %13, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

43:                                               ; preds = %19
  %44 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %14, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

45:                                               ; preds = %19
  %46 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %15, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

47:                                               ; preds = %19
  %48 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %16, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

49:                                               ; preds = %19
  %50 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %17, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

51:                                               ; preds = %19
  %52 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %18, ptr addrspace(2) readonly captures(none) @__air_sampler_state.5, <2 x float> %1, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #5
  br label %53

53:                                               ; preds = %51, %49, %47, %45, %43, %41, %39, %37, %35, %33, %31, %29, %27, %25, %23, %21
  %54 = phi { <4 x float>, i8 } [ %52, %51 ], [ %50, %49 ], [ %48, %47 ], [ %46, %45 ], [ %44, %43 ], [ %42, %41 ], [ %40, %39 ], [ %38, %37 ], [ %36, %35 ], [ %34, %33 ], [ %32, %31 ], [ %30, %29 ], [ %28, %27 ], [ %26, %25 ], [ %24, %23 ], [ %22, %21 ]
  %55 = extractvalue { <4 x float>, i8 } %54, 0
  ret <4 x float> %55
}

attributes #0 = { norecurse nounwind memory(argmem: readwrite) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { nounwind memory(none) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="0" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #2 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #3 = { norecurse nounwind memory(none) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #4 = { convergent nounwind memory(argmem: read) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #5 = { convergent nounwind memory(argmem: read) }
attributes #6 = { convergent nounwind memory(read) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #7 = { nounwind memory(argmem: read) }
attributes #8 = { nounwind memory(none) }

!llvm.ident = !{!0}
!air.version = !{!1}
!air.language_version = !{!2}
!air.compile_options = !{!3, !4, !5}
!air.kernel = !{!6}
!llvm.module.flags = !{!11, !12, !13, !14, !15, !16, !17, !18}
!air.vertex = !{!19, !24}
!air.fragment = !{!27, !30, !35, !36, !37, !40, !44, !47, !50, !53, !60}
!air.sampler_states = !{!79, !80, !81, !82, !83, !84}

!0 = !{!"Apple metal version 32023.157 (metalfe-32023.157)"}
!1 = !{i32 2, i32 5, i32 0}
!2 = !{!"Metal", i32 3, i32 0, i32 0}
!3 = !{!"air.compile.denorms_disable"}
!4 = !{!"air.compile.fast_math_enable"}
!5 = !{!"air.compile.framebuffer_fetch_enable"}
!6 = !{ptr @fill, !7, !8}
!7 = !{}
!8 = !{!9, !10}
!9 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"words"}
!10 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!11 = !{i32 2, !"SDK Version", [2 x i32] [i32 17, i32 5]}
!12 = !{i32 1, !"wchar_size", i32 4}
!13 = !{i32 7, !"air.max_device_buffers", i32 31}
!14 = !{i32 7, !"air.max_constant_buffers", i32 31}
!15 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!16 = !{i32 7, !"air.max_textures", i32 128}
!17 = !{i32 7, !"air.max_read_write_textures", i32 8}
!18 = !{i32 7, !"air.max_samplers", i32 16}
!19 = !{ptr @tri_vs, !20, !22}
!20 = !{!21}
!21 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!22 = !{!23}
!23 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!24 = !{ptr @quad_vs, !25, !22}
!25 = !{!21, !26}
!26 = !{!"air.vertex_output", !"generated(2uvDv2_f)", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!27 = !{ptr @tri_fs, !28, !7}
!28 = !{!29}
!29 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!30 = !{ptr @tex_fs, !28, !31}
!31 = !{!32, !33, !34}
!32 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position", !"air.arg_unused"}
!33 = !{i32 1, !"air.fragment_input", !"generated(2uvDv2_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!34 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!35 = !{ptr @tex_lin_fs, !28, !31}
!36 = !{ptr @tex_px_fs, !28, !31}
!37 = !{ptr @tex3_fs, !28, !38}
!38 = !{!32, !33, !39}
!39 = !{i32 2, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!40 = !{ptr @sweep_fs, !28, !41}
!41 = !{!32, !33, !34, !42, !43}
!42 = !{i32 3, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"s"}
!43 = !{i32 4, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"float2", !"air.arg_name", !"scale"}
!44 = !{ptr @solid_fs, !28, !45}
!45 = !{!32, !46}
!46 = !{i32 1, !"air.fragment_input", !"generated(2uvDv2_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv", !"air.arg_unused"}
!47 = !{ptr @fetch_fs, !28, !48}
!48 = !{!32, !46, !49}
!49 = !{i32 2, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"dst"}
!50 = !{ptr @fetch_tex_fs, !28, !51}
!51 = !{!32, !33, !49, !52}
!52 = !{i32 3, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t"}
!53 = !{ptr @fetch2_fs, !54, !57}
!54 = !{!55, !56}
!55 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c0"}
!56 = !{!"air.render_target", i32 1, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"c1"}
!57 = !{!32, !46, !58, !59}
!58 = !{i32 2, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"d0"}
!59 = !{i32 3, !"air.render_target", i32 1, !"air.arg_type_name", !"float4", !"air.arg_name", !"d1"}
!60 = !{ptr @index_fs, !28, !61}
!61 = !{!32, !33, !62, !63, !64, !65, !66, !67, !68, !69, !70, !71, !72, !73, !74, !75, !76, !77, !78}
!62 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"which"}
!63 = !{i32 3, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t0"}
!64 = !{i32 4, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t1"}
!65 = !{i32 5, !"air.texture", !"air.location_index", i32 2, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t2"}
!66 = !{i32 6, !"air.texture", !"air.location_index", i32 3, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t3"}
!67 = !{i32 7, !"air.texture", !"air.location_index", i32 4, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t4"}
!68 = !{i32 8, !"air.texture", !"air.location_index", i32 5, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t5"}
!69 = !{i32 9, !"air.texture", !"air.location_index", i32 6, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t6"}
!70 = !{i32 10, !"air.texture", !"air.location_index", i32 7, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t7"}
!71 = !{i32 11, !"air.texture", !"air.location_index", i32 8, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t8"}
!72 = !{i32 12, !"air.texture", !"air.location_index", i32 9, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t9"}
!73 = !{i32 13, !"air.texture", !"air.location_index", i32 10, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t10"}
!74 = !{i32 14, !"air.texture", !"air.location_index", i32 11, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t11"}
!75 = !{i32 15, !"air.texture", !"air.location_index", i32 12, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t12"}
!76 = !{i32 16, !"air.texture", !"air.location_index", i32 13, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t13"}
!77 = !{i32 17, !"air.texture", !"air.location_index", i32 14, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t14"}
!78 = !{i32 18, !"air.texture", !"air.location_index", i32 15, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"t15"}
!79 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
!80 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.1}
!81 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.2}
!82 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.3}
!83 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.1.4}
!84 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state.5}
!85 = !{!86, !86, i64 0}
!86 = !{!"int", !87, i64 0}
!87 = !{!"omnipotent char", !88, i64 0}
!88 = !{!"Simple C++ TBAA"}
!89 = !{!90}
!90 = distinct !{!90, !91, !"air-alias-scope-arg(0)"}
!91 = distinct !{!91, !"air-alias-scopes(fill)"}
!92 = !{!87, !87, i64 0}
!93 = !{!94}
!94 = distinct !{!94, !95, !"air-alias-scope-textures"}
!95 = distinct !{!95, !"air-alias-scopes(tex_px_fs)"}
!96 = !{!97}
!97 = distinct !{!97, !98, !"air-alias-scope-arg(4)"}
!98 = distinct !{!98, !"air-alias-scopes(sweep_fs)"}
!99 = !{!100, !101}
!100 = distinct !{!100, !98, !"air-alias-scope-textures"}
!101 = distinct !{!101, !98, !"air-alias-scope-samplers"}
!102 = !{!103}
!103 = distinct !{!103, !104, !"air-alias-scope-arg(2)"}
!104 = distinct !{!104, !"air-alias-scopes(index_fs)"}
!105 = !{!106}
!106 = distinct !{!106, !104, !"air-alias-scope-textures"}
