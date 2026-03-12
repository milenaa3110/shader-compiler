; ModuleID = 'llvm-link'
source_filename = "llvm-link"
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-linux-gnu"

%VS_Output = type { <4 x float>, <2 x float>, <4 x float> }
%FS_Output = type { <4 x float> }

@uTime = global float 0.000000e+00
@vs_total_floats = constant i32 10
@vs_varying_floats = constant i32 6
@fs_output_floats = constant i32 4

define void @vs_main(i32 %gl_VertexID, i32 %gl_InstanceID, ptr %_out) #0 !shader.stage !0 {
entry:
  %uv = alloca float, align 4
  %uu = alloca float, align 4
  %uo = alloca float, align 4
  %py = alloca float, align 4
  %px = alloca float, align 4
  %vid = alloca float, align 4
  %vColor = alloca <4 x float>, align 16
  %vUV = alloca <2 x float>, align 8
  %gl_Position = alloca <4 x float>, align 16
  %_out3 = alloca ptr, align 8
  %gl_InstanceID2 = alloca i32, align 4
  %gl_VertexID1 = alloca i32, align 4
  store i32 %gl_VertexID, ptr %gl_VertexID1, align 4
  store i32 %gl_InstanceID, ptr %gl_InstanceID2, align 4
  store ptr %_out, ptr %_out3, align 8
  store <4 x float> zeroinitializer, ptr %gl_Position, align 16
  store <2 x float> zeroinitializer, ptr %vUV, align 8
  store <4 x float> zeroinitializer, ptr %vColor, align 16
  %gl_VertexID4 = load i32, ptr %gl_VertexID1, align 4
  %sitofp = sitofp i32 %gl_VertexID4 to float
  store float %sitofp, ptr %vid, align 4
  store float 0.000000e+00, ptr %px, align 4
  store float 0.000000e+00, ptr %py, align 4
  %vid5 = load float, ptr %vid, align 4
  %cmptmp = fcmp oeq float %vid5, 0.000000e+00
  br i1 %cmptmp, label %then, label %ifend

then:                                             ; preds = %entry
  store float -1.000000e+00, ptr %px, align 4
  store float -1.000000e+00, ptr %py, align 4
  br label %ifend

ifend:                                            ; preds = %then, %entry
  %vid6 = load float, ptr %vid, align 4
  %cmptmp7 = fcmp oeq float %vid6, 1.000000e+00
  br i1 %cmptmp7, label %then8, label %ifend9

then8:                                            ; preds = %ifend
  store float 1.000000e+00, ptr %px, align 4
  store float -1.000000e+00, ptr %py, align 4
  br label %ifend9

ifend9:                                           ; preds = %then8, %ifend
  %vid10 = load float, ptr %vid, align 4
  %cmptmp11 = fcmp oeq float %vid10, 2.000000e+00
  br i1 %cmptmp11, label %then12, label %ifend13

then12:                                           ; preds = %ifend9
  store float 1.000000e+00, ptr %px, align 4
  store float 1.000000e+00, ptr %py, align 4
  br label %ifend13

ifend13:                                          ; preds = %then12, %ifend9
  %vid14 = load float, ptr %vid, align 4
  %cmptmp15 = fcmp oeq float %vid14, 3.000000e+00
  br i1 %cmptmp15, label %then16, label %ifend17

then16:                                           ; preds = %ifend13
  store float -1.000000e+00, ptr %px, align 4
  store float -1.000000e+00, ptr %py, align 4
  br label %ifend17

ifend17:                                          ; preds = %then16, %ifend13
  %vid18 = load float, ptr %vid, align 4
  %cmptmp19 = fcmp oeq float %vid18, 4.000000e+00
  br i1 %cmptmp19, label %then20, label %ifend21

then20:                                           ; preds = %ifend17
  store float 1.000000e+00, ptr %px, align 4
  store float 1.000000e+00, ptr %py, align 4
  br label %ifend21

ifend21:                                          ; preds = %then20, %ifend17
  %vid22 = load float, ptr %vid, align 4
  %cmptmp23 = fcmp oeq float %vid22, 5.000000e+00
  br i1 %cmptmp23, label %then24, label %ifend25

then24:                                           ; preds = %ifend21
  store float -1.000000e+00, ptr %px, align 4
  store float 1.000000e+00, ptr %py, align 4
  br label %ifend25

ifend25:                                          ; preds = %then24, %ifend21
  %uTime = load float, ptr @uTime, align 4
  %multmp = fmul float %uTime, 0x3FC99999A0000000
  store float %multmp, ptr %uo, align 4
  %px26 = load float, ptr %px, align 4
  %addtmp = fadd float %px26, 1.000000e+00
  %multmp27 = fmul float %addtmp, 5.000000e-01
  %uo28 = load float, ptr %uo, align 4
  %addtmp29 = fadd float %multmp27, %uo28
  store float %addtmp29, ptr %uu, align 4
  %py30 = load float, ptr %py, align 4
  %addtmp31 = fadd float %py30, 1.000000e+00
  %multmp32 = fmul float %addtmp31, 5.000000e-01
  %uTime33 = load float, ptr @uTime, align 4
  %multmp34 = fmul float %uTime33, 0x3FC3333340000000
  %addtmp35 = fadd float %multmp32, %multmp34
  store float %addtmp35, ptr %uv, align 4
  %px36 = load float, ptr %px, align 4
  %py37 = load float, ptr %py, align 4
  %ins = insertelement <4 x float> undef, float %px36, i32 0
  %ins38 = insertelement <4 x float> %ins, float %py37, i32 1
  %ins39 = insertelement <4 x float> %ins38, float 0.000000e+00, i32 2
  %ins40 = insertelement <4 x float> %ins39, float 1.000000e+00, i32 3
  store <4 x float> %ins40, ptr %gl_Position, align 16
  %uu41 = load float, ptr %uu, align 4
  %uv42 = load float, ptr %uv, align 4
  %ins43 = insertelement <2 x float> undef, float %uu41, i32 0
  %ins44 = insertelement <2 x float> %ins43, float %uv42, i32 1
  store <2 x float> %ins44, ptr %vUV, align 8
  %uTime45 = load float, ptr @uTime, align 4
  %vid46 = load float, ptr %vid, align 4
  %addtmp47 = fadd float %uTime45, %vid46
  %0 = call float @llvm.sin.f32(float %addtmp47)
  %multmp48 = fmul float 5.000000e-01, %0
  %addtmp49 = fadd float 5.000000e-01, %multmp48
  %uTime50 = load float, ptr @uTime, align 4
  %1 = call float @llvm.cos.f32(float %uTime50)
  %multmp51 = fmul float 5.000000e-01, %1
  %addtmp52 = fadd float 5.000000e-01, %multmp51
  %ins53 = insertelement <4 x float> undef, float %addtmp49, i32 0
  %ins54 = insertelement <4 x float> %ins53, float %addtmp52, i32 1
  %ins55 = insertelement <4 x float> %ins54, float 5.000000e-01, i32 2
  %ins56 = insertelement <4 x float> %ins55, float 1.000000e+00, i32 3
  store <4 x float> %ins56, ptr %vColor, align 16
  %out.ptr = load ptr, ptr %_out3, align 8
  %gl_Position.ld = load <4 x float>, ptr %gl_Position, align 16
  %2 = getelementptr inbounds %VS_Output, ptr %out.ptr, i32 0, i32 0
  store <4 x float> %gl_Position.ld, ptr %2, align 16
  %vUV.ld = load <2 x float>, ptr %vUV, align 8
  %3 = getelementptr inbounds %VS_Output, ptr %out.ptr, i32 0, i32 1
  store <2 x float> %vUV.ld, ptr %3, align 8
  %vColor.ld = load <4 x float>, ptr %vColor, align 16
  %4 = getelementptr inbounds %VS_Output, ptr %out.ptr, i32 0, i32 2
  store <4 x float> %vColor.ld, ptr %4, align 16
  ret void
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.sin.f32(float) #1

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.cos.f32(float) #1

define void @vs_invoke(i32 %vid, i32 %iid, ptr %flat_out) #0 {
entry:
  %vs_out = alloca %VS_Output, align 16
  call void @vs_main(i32 %vid, i32 %iid, ptr %vs_out)
  %fld = getelementptr inbounds %VS_Output, ptr %vs_out, i32 0, i32 0
  %vec = load <4 x float>, ptr %fld, align 16
  %0 = extractelement <4 x float> %vec, i32 0
  %dst = getelementptr inbounds float, ptr %flat_out, i32 0
  store float %0, ptr %dst, align 4
  %1 = extractelement <4 x float> %vec, i32 1
  %dst1 = getelementptr inbounds float, ptr %flat_out, i32 1
  store float %1, ptr %dst1, align 4
  %2 = extractelement <4 x float> %vec, i32 2
  %dst2 = getelementptr inbounds float, ptr %flat_out, i32 2
  store float %2, ptr %dst2, align 4
  %3 = extractelement <4 x float> %vec, i32 3
  %dst3 = getelementptr inbounds float, ptr %flat_out, i32 3
  store float %3, ptr %dst3, align 4
  %fld4 = getelementptr inbounds %VS_Output, ptr %vs_out, i32 0, i32 1
  %vec5 = load <2 x float>, ptr %fld4, align 8
  %4 = extractelement <2 x float> %vec5, i32 0
  %dst6 = getelementptr inbounds float, ptr %flat_out, i32 4
  store float %4, ptr %dst6, align 4
  %5 = extractelement <2 x float> %vec5, i32 1
  %dst7 = getelementptr inbounds float, ptr %flat_out, i32 5
  store float %5, ptr %dst7, align 4
  %fld8 = getelementptr inbounds %VS_Output, ptr %vs_out, i32 0, i32 2
  %vec9 = load <4 x float>, ptr %fld8, align 16
  %6 = extractelement <4 x float> %vec9, i32 0
  %dst10 = getelementptr inbounds float, ptr %flat_out, i32 6
  store float %6, ptr %dst10, align 4
  %7 = extractelement <4 x float> %vec9, i32 1
  %dst11 = getelementptr inbounds float, ptr %flat_out, i32 7
  store float %7, ptr %dst11, align 4
  %8 = extractelement <4 x float> %vec9, i32 2
  %dst12 = getelementptr inbounds float, ptr %flat_out, i32 8
  store float %8, ptr %dst12, align 4
  %9 = extractelement <4 x float> %vec9, i32 3
  %dst13 = getelementptr inbounds float, ptr %flat_out, i32 9
  store float %9, ptr %dst13, align 4
  ret void
}

define void @fs_main(<4 x float> %gl_FragCoord, <2 x float> %vUV, <4 x float> %vColor, ptr %_out) #0 !shader.stage !1 {
entry:
  %b = alloca float, align 4
  %g = alloca float, align 4
  %r = alloca float, align 4
  %p = alloca float, align 4
  %v = alloca float, align 4
  %u = alloca float, align 4
  %FragColor = alloca <4 x float>, align 16
  %_out4 = alloca ptr, align 8
  %vColor3 = alloca <4 x float>, align 16
  %vUV2 = alloca <2 x float>, align 8
  %gl_FragCoord1 = alloca <4 x float>, align 16
  store <4 x float> %gl_FragCoord, ptr %gl_FragCoord1, align 16
  store <2 x float> %vUV, ptr %vUV2, align 8
  store <4 x float> %vColor, ptr %vColor3, align 16
  store ptr %_out, ptr %_out4, align 8
  store <4 x float> zeroinitializer, ptr %FragColor, align 16
  %vUV5 = load <2 x float>, ptr %vUV2, align 8
  %comp = extractelement <2 x float> %vUV5, i32 0
  store float %comp, ptr %u, align 4
  %vUV6 = load <2 x float>, ptr %vUV2, align 8
  %comp7 = extractelement <2 x float> %vUV6, i32 1
  store float %comp7, ptr %v, align 4
  store float 0.000000e+00, ptr %p, align 4
  %p8 = load float, ptr %p, align 4
  %u9 = load float, ptr %u, align 4
  %multmp = fmul float %u9, 0x401921FF20000000
  %0 = call float @llvm.sin.f32(float %multmp)
  %addtmp = fadd float %p8, %0
  store float %addtmp, ptr %p, align 4
  %p10 = load float, ptr %p, align 4
  %v11 = load float, ptr %v, align 4
  %multmp12 = fmul float %v11, 0x401921FF20000000
  %1 = call float @llvm.sin.f32(float %multmp12)
  %addtmp13 = fadd float %p10, %1
  store float %addtmp13, ptr %p, align 4
  %p14 = load float, ptr %p, align 4
  %u15 = load float, ptr %u, align 4
  %v16 = load float, ptr %v, align 4
  %addtmp17 = fadd float %u15, %v16
  %multmp18 = fmul float %addtmp17, 4.000000e+00
  %2 = call float @llvm.sin.f32(float %multmp18)
  %addtmp19 = fadd float %p14, %2
  store float %addtmp19, ptr %p, align 4
  %p20 = load float, ptr %p, align 4
  %u21 = load float, ptr %u, align 4
  %u22 = load float, ptr %u, align 4
  %multmp23 = fmul float %u21, %u22
  %v24 = load float, ptr %v, align 4
  %v25 = load float, ptr %v, align 4
  %multmp26 = fmul float %v24, %v25
  %addtmp27 = fadd float %multmp23, %multmp26
  %addtmp28 = fadd float %addtmp27, 0x3F847AE140000000
  %sqrt = call float @llvm.sqrt.f32(float %addtmp28)
  %multmp29 = fmul float %sqrt, 8.000000e+00
  %3 = call float @llvm.sin.f32(float %multmp29)
  %addtmp30 = fadd float %p20, %3
  store float %addtmp30, ptr %p, align 4
  %p31 = load float, ptr %p, align 4
  %addtmp32 = fadd float %p31, 4.000000e+00
  %multmp33 = fmul float %addtmp32, 1.250000e-01
  store float %multmp33, ptr %p, align 4
  %p34 = load float, ptr %p, align 4
  %multmp35 = fmul float %p34, 0x401921FF20000000
  %addtmp36 = fadd float %multmp35, 0.000000e+00
  %4 = call float @llvm.cos.f32(float %addtmp36)
  %multmp37 = fmul float 5.000000e-01, %4
  %vColor38 = load <4 x float>, ptr %vColor3, align 16
  %comp39 = extractelement <4 x float> %vColor38, i32 0
  %multmp40 = fmul float %multmp37, %comp39
  %addtmp41 = fadd float 5.000000e-01, %multmp40
  store float %addtmp41, ptr %r, align 4
  %p42 = load float, ptr %p, align 4
  %multmp43 = fmul float %p42, 0x401921FF20000000
  %addtmp44 = fadd float %multmp43, 0x4000B851E0000000
  %5 = call float @llvm.cos.f32(float %addtmp44)
  %multmp45 = fmul float 5.000000e-01, %5
  %vColor46 = load <4 x float>, ptr %vColor3, align 16
  %comp47 = extractelement <4 x float> %vColor46, i32 1
  %multmp48 = fmul float %multmp45, %comp47
  %addtmp49 = fadd float 5.000000e-01, %multmp48
  store float %addtmp49, ptr %g, align 4
  %p50 = load float, ptr %p, align 4
  %multmp51 = fmul float %p50, 0x401921FF20000000
  %addtmp52 = fadd float %multmp51, 0x4010C28F60000000
  %6 = call float @llvm.cos.f32(float %addtmp52)
  %multmp53 = fmul float 5.000000e-01, %6
  %vColor54 = load <4 x float>, ptr %vColor3, align 16
  %comp55 = extractelement <4 x float> %vColor54, i32 2
  %multmp56 = fmul float %multmp53, %comp55
  %addtmp57 = fadd float 5.000000e-01, %multmp56
  store float %addtmp57, ptr %b, align 4
  %r58 = load float, ptr %r, align 4
  %g59 = load float, ptr %g, align 4
  %b60 = load float, ptr %b, align 4
  %ins = insertelement <4 x float> undef, float %r58, i32 0
  %ins61 = insertelement <4 x float> %ins, float %g59, i32 1
  %ins62 = insertelement <4 x float> %ins61, float %b60, i32 2
  %ins63 = insertelement <4 x float> %ins62, float 1.000000e+00, i32 3
  store <4 x float> %ins63, ptr %FragColor, align 16
  %out.ptr = load ptr, ptr %_out4, align 8
  %FragColor.ld = load <4 x float>, ptr %FragColor, align 16
  %7 = getelementptr inbounds %FS_Output, ptr %out.ptr, i32 0, i32 0
  store <4 x float> %FragColor.ld, ptr %7, align 16
  ret void
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.sqrt.f32(float) #1

define void @fs_invoke(ptr %fragcoord, ptr %varyings, ptr %flat_out) #0 {
entry:
  %fragcoord_vec = load <4 x float>, ptr %fragcoord, align 16
  %src = getelementptr inbounds float, ptr %varyings, i32 0
  %e = load float, ptr %src, align 4
  %0 = insertelement <2 x float> undef, float %e, i32 0
  %src1 = getelementptr inbounds float, ptr %varyings, i32 1
  %e2 = load float, ptr %src1, align 4
  %1 = insertelement <2 x float> %0, float %e2, i32 1
  %src3 = getelementptr inbounds float, ptr %varyings, i32 2
  %e4 = load float, ptr %src3, align 4
  %2 = insertelement <4 x float> undef, float %e4, i32 0
  %src5 = getelementptr inbounds float, ptr %varyings, i32 3
  %e6 = load float, ptr %src5, align 4
  %3 = insertelement <4 x float> %2, float %e6, i32 1
  %src7 = getelementptr inbounds float, ptr %varyings, i32 4
  %e8 = load float, ptr %src7, align 4
  %4 = insertelement <4 x float> %3, float %e8, i32 2
  %src9 = getelementptr inbounds float, ptr %varyings, i32 5
  %e10 = load float, ptr %src9, align 4
  %5 = insertelement <4 x float> %4, float %e10, i32 3
  %fs_out = alloca %FS_Output, align 16
  call void @fs_main(<4 x float> %fragcoord_vec, <2 x float> %1, <4 x float> %5, ptr %fs_out)
  %fld = getelementptr inbounds %FS_Output, ptr %fs_out, i32 0, i32 0
  %vec = load <4 x float>, ptr %fld, align 16
  %6 = extractelement <4 x float> %vec, i32 0
  %dst = getelementptr inbounds float, ptr %flat_out, i32 0
  store float %6, ptr %dst, align 4
  %7 = extractelement <4 x float> %vec, i32 1
  %dst11 = getelementptr inbounds float, ptr %flat_out, i32 1
  store float %7, ptr %dst11, align 4
  %8 = extractelement <4 x float> %vec, i32 2
  %dst12 = getelementptr inbounds float, ptr %flat_out, i32 2
  store float %8, ptr %dst12, align 4
  %9 = extractelement <4 x float> %vec, i32 3
  %dst13 = getelementptr inbounds float, ptr %flat_out, i32 3
  store float %9, ptr %dst13, align 4
  ret void
}

attributes #0 = { "target-cpu"="generic-rv64" "target-features"="+m,+a,+f,+d,+v,+zve64f" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!0 = !{!"vertex"}
!1 = !{!"fragment"}
