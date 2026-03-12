; ModuleID = 'shader_module'
source_filename = "shader_module"

%VS_Output = type { <4 x float>, <2 x float>, <4 x float> }

@uTime = global float 0.000000e+00
@vs_total_floats = constant i32 10
@vs_varying_floats = constant i32 6

define void @vs_main(i32 %gl_VertexID, i32 %gl_InstanceID, ptr %_out) !shader.stage !0 {
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
declare float @llvm.sin.f32(float) #0

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.cos.f32(float) #0

define void @vs_invoke(i32 %vid, i32 %iid, ptr %flat_out) {
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

attributes #0 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!0 = !{!"vertex"}
