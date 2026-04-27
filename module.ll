; ModuleID = 'shader_module'
source_filename = "shader_module"
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-linux-gnu"

%FS_Output = type { <4 x float> }

@uTime = global float 0.000000e+00
@fs_output_floats = constant i32 4

define void @fs_main(<4 x float> %gl_FragCoord, <2 x float> %vUV, ptr %_out) #0 !shader.stage !0 {
entry:
  %b = alloca float, align 4
  %g = alloca float, align 4
  %r = alloca float, align 4
  %t = alloca float, align 4
  %inside = alloca float, align 4
  %ny = alloca float, align 4
  %nx = alloca float, align 4
  %i = alloca float, align 4
  %iter = alloca float, align 4
  %zy = alloca float, align 4
  %zx = alloca float, align 4
  %cy = alloca float, align 4
  %cx = alloca float, align 4
  %zoom = alloca float, align 4
  %FragColor = alloca <4 x float>, align 16
  %_out3 = alloca ptr, align 8
  %vUV2 = alloca <2 x float>, align 8
  %gl_FragCoord1 = alloca <4 x float>, align 16
  store <4 x float> %gl_FragCoord, ptr %gl_FragCoord1, align 16
  store <2 x float> %vUV, ptr %vUV2, align 8
  store ptr %_out, ptr %_out3, align 8
  store <4 x float> zeroinitializer, ptr %FragColor, align 16
  %uTime = load float, ptr @uTime, align 4
  %neg = fneg float %uTime
  %multmp = fmul float %neg, 2.500000e-01
  %addtmp = fadd float %multmp, 5.000000e-01
  %0 = call float @llvm.exp.f32(float %addtmp)
  store float %0, ptr %zoom, align 4
  %vUV4 = load <2 x float>, ptr %vUV2, align 8
  %comp = extractelement <2 x float> %vUV4, i32 0
  %subtmp = fsub float %comp, 5.000000e-01
  %multmp5 = fmul float %subtmp, 0x4004CCCCC0000000
  %zoom6 = load float, ptr %zoom, align 4
  %multmp7 = fmul float %multmp5, %zoom6
  %subtmp8 = fsub float %multmp7, 0x3FE7CB9240000000
  store float %subtmp8, ptr %cx, align 4
  %vUV9 = load <2 x float>, ptr %vUV2, align 8
  %comp10 = extractelement <2 x float> %vUV9, i32 1
  %subtmp11 = fsub float %comp10, 5.000000e-01
  %multmp12 = fmul float %subtmp11, 0x4004CCCCC0000000
  %zoom13 = load float, ptr %zoom, align 4
  %multmp14 = fmul float %multmp12, %zoom13
  %addtmp15 = fadd float %multmp14, 0x3FC0E21960000000
  store float %addtmp15, ptr %cy, align 4
  store float 0.000000e+00, ptr %zx, align 4
  store float 0.000000e+00, ptr %zy, align 4
  store float 0.000000e+00, ptr %iter, align 4
  store float 0.000000e+00, ptr %i, align 4
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %i16 = load float, ptr %i, align 4
  %cmptmp = fcmp olt float %i16, 8.000000e+01
  br i1 %cmptmp, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %zx17 = load float, ptr %zx, align 4
  %zx18 = load float, ptr %zx, align 4
  %multmp19 = fmul float %zx17, %zx18
  %zy20 = load float, ptr %zy, align 4
  %zy21 = load float, ptr %zy, align 4
  %multmp22 = fmul float %zy20, %zy21
  %addtmp23 = fadd float %multmp19, %multmp22
  %cmptmp24 = fcmp olt float %addtmp23, 4.000000e+00
  br i1 %cmptmp24, label %then, label %ifend

for.inc:                                          ; preds = %ifend
  %i44 = load float, ptr %i, align 4
  %addtmp45 = fadd float %i44, 1.000000e+00
  store float %addtmp45, ptr %i, align 4
  br label %for.cond

for.end:                                          ; preds = %for.cond
  %zx46 = load float, ptr %zx, align 4
  %zx47 = load float, ptr %zx, align 4
  %multmp48 = fmul float %zx46, %zx47
  %zy49 = load float, ptr %zy, align 4
  %zy50 = load float, ptr %zy, align 4
  %multmp51 = fmul float %zy49, %zy50
  %addtmp52 = fadd float %multmp48, %multmp51
  %1 = fcmp olt float %addtmp52, 0x400FEB8520000000
  %step = select i1 %1, float 0.000000e+00, float 1.000000e+00
  store float %step, ptr %inside, align 4
  %inside53 = load float, ptr %inside, align 4
  %subtmp54 = fsub float 1.000000e+00, %inside53
  store float %subtmp54, ptr %inside, align 4
  %iter55 = load float, ptr %iter, align 4
  %divtmp = fdiv float %iter55, 8.000000e+01
  %uTime56 = load float, ptr @uTime, align 4
  %multmp57 = fmul float %uTime56, 0x3FB99999A0000000
  %addtmp58 = fadd float %divtmp, %multmp57
  store float %addtmp58, ptr %t, align 4
  %t59 = load float, ptr %t, align 4
  %multmp60 = fmul float %t59, 0x401921FF20000000
  %addtmp61 = fadd float %multmp60, 0.000000e+00
  %2 = call float @llvm.cos.f32(float %addtmp61)
  %multmp62 = fmul float 5.000000e-01, %2
  %addtmp63 = fadd float 5.000000e-01, %multmp62
  store float %addtmp63, ptr %r, align 4
  %t64 = load float, ptr %t, align 4
  %multmp65 = fmul float %t64, 0x401921FF20000000
  %addtmp66 = fadd float %multmp65, 0x4000B851E0000000
  %3 = call float @llvm.cos.f32(float %addtmp66)
  %multmp67 = fmul float 5.000000e-01, %3
  %addtmp68 = fadd float 5.000000e-01, %multmp67
  store float %addtmp68, ptr %g, align 4
  %t69 = load float, ptr %t, align 4
  %multmp70 = fmul float %t69, 0x401921FF20000000
  %addtmp71 = fadd float %multmp70, 0x4010C28F60000000
  %4 = call float @llvm.cos.f32(float %addtmp71)
  %multmp72 = fmul float 5.000000e-01, %4
  %addtmp73 = fadd float 5.000000e-01, %multmp72
  store float %addtmp73, ptr %b, align 4
  %r74 = load float, ptr %r, align 4
  %inside75 = load float, ptr %inside, align 4
  %multmp76 = fmul float %r74, %inside75
  %g77 = load float, ptr %g, align 4
  %inside78 = load float, ptr %inside, align 4
  %multmp79 = fmul float %g77, %inside78
  %b80 = load float, ptr %b, align 4
  %inside81 = load float, ptr %inside, align 4
  %multmp82 = fmul float %b80, %inside81
  %ins = insertelement <4 x float> undef, float %multmp76, i32 0
  %ins83 = insertelement <4 x float> %ins, float %multmp79, i32 1
  %ins84 = insertelement <4 x float> %ins83, float %multmp82, i32 2
  %ins85 = insertelement <4 x float> %ins84, float 1.000000e+00, i32 3
  store <4 x float> %ins85, ptr %FragColor, align 16
  %out.ptr = load ptr, ptr %_out3, align 8
  %FragColor.ld = load <4 x float>, ptr %FragColor, align 16
  %5 = getelementptr inbounds %FS_Output, ptr %out.ptr, i32 0, i32 0
  store <4 x float> %FragColor.ld, ptr %5, align 16
  ret void

then:                                             ; preds = %for.body
  %zx25 = load float, ptr %zx, align 4
  %zx26 = load float, ptr %zx, align 4
  %multmp27 = fmul float %zx25, %zx26
  %zy28 = load float, ptr %zy, align 4
  %zy29 = load float, ptr %zy, align 4
  %multmp30 = fmul float %zy28, %zy29
  %subtmp31 = fsub float %multmp27, %multmp30
  %cx32 = load float, ptr %cx, align 4
  %addtmp33 = fadd float %subtmp31, %cx32
  store float %addtmp33, ptr %nx, align 4
  %zx34 = load float, ptr %zx, align 4
  %multmp35 = fmul float 2.000000e+00, %zx34
  %zy36 = load float, ptr %zy, align 4
  %multmp37 = fmul float %multmp35, %zy36
  %cy38 = load float, ptr %cy, align 4
  %addtmp39 = fadd float %multmp37, %cy38
  store float %addtmp39, ptr %ny, align 4
  %nx40 = load float, ptr %nx, align 4
  store float %nx40, ptr %zx, align 4
  %ny41 = load float, ptr %ny, align 4
  store float %ny41, ptr %zy, align 4
  %i42 = load float, ptr %i, align 4
  %addtmp43 = fadd float %i42, 1.000000e+00
  store float %addtmp43, ptr %iter, align 4
  br label %ifend

ifend:                                            ; preds = %then, %for.body
  br label %for.inc
}

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.exp.f32(float) #1

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.cos.f32(float) #1

define void @fs_invoke(ptr %fragcoord, ptr %varyings, ptr %flat_out) #0 {
entry:
  %fragcoord_vec = load <4 x float>, ptr %fragcoord, align 16
  %src = getelementptr inbounds float, ptr %varyings, i32 0
  %e = load float, ptr %src, align 4
  %0 = insertelement <2 x float> undef, float %e, i32 0
  %src1 = getelementptr inbounds float, ptr %varyings, i32 1
  %e2 = load float, ptr %src1, align 4
  %1 = insertelement <2 x float> %0, float %e2, i32 1
  %fs_out = alloca %FS_Output, align 16
  call void @fs_main(<4 x float> %fragcoord_vec, <2 x float> %1, ptr %fs_out)
  %fld = getelementptr inbounds %FS_Output, ptr %fs_out, i32 0, i32 0
  %vec = load <4 x float>, ptr %fld, align 16
  %2 = extractelement <4 x float> %vec, i32 0
  %dst = getelementptr inbounds float, ptr %flat_out, i32 0
  store float %2, ptr %dst, align 4
  %3 = extractelement <4 x float> %vec, i32 1
  %dst3 = getelementptr inbounds float, ptr %flat_out, i32 1
  store float %3, ptr %dst3, align 4
  %4 = extractelement <4 x float> %vec, i32 2
  %dst4 = getelementptr inbounds float, ptr %flat_out, i32 2
  store float %4, ptr %dst4, align 4
  %5 = extractelement <4 x float> %vec, i32 3
  %dst5 = getelementptr inbounds float, ptr %flat_out, i32 3
  store float %5, ptr %dst5, align 4
  ret void
}

attributes #0 = { "target-cpu"="generic-rv64" "target-features"="+m,+a,+f,+d,+v,+zve64f" }
attributes #1 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }

!0 = !{!"fragment"}
