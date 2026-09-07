#!/usr/bin/env python3
"""Regression checks for benchmark options, metrics, and actual no-video hosts."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import benchmark_pipeline as bench

ROOT = Path(__file__).resolve().parents[2]


class Metrics(unittest.TestCase):
    def test_timestamp_reset_wrap_conversion_and_availability(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "timer"
            subprocess.run(["g++", "-std=c++17", str(ROOT / "test/test_vulkan_benchmark_timer.cpp"), "-o", str(binary)], check=True)
            output = subprocess.check_output([str(binary)], text=True)
            self.assertIn("Vulkan device avg: 5.000000000 ms/frame", output)
            self.assertEqual(output.count("Vulkan device avg: unavailable"), 2)

    def test_never_substitute_host_time(self):
        text = "Vulkan host avg: 23 ms/frame\nVulkan device avg: unavailable\n"
        sample = bench.parse_sample(text, True)
        self.assertIsNone(sample["ms"])
        self.assertEqual(sample["host_ms"], 23)

    def test_device_and_stage_identity(self):
        text = "Vulkan device: llvmpipe (type=CPU)\nVulkan device avg: 1e-3 ms/frame\n"
        self.assertEqual(bench.parse_sample(text, True)["ms"], .001)
        self.assertIn("CPU", bench.parse_sample(text, True)["device"])
        text = "[pipeline] mode=packet VS=scalar FS=packet packet_width=8 fallback=mixed\nRISC-V avg: 2 ms/frame"
        self.assertEqual(bench.parse_sample(text, False)["stages"]["fallback"], "mixed")

    def test_complete_warmup_alternating_order_and_fallback(self):
        calls = []
        def execute(command, log, env):
            calls.append(command)
            mode = command[0]
            return (f"[pipeline] mode={mode} VS=scalar FS=scalar packet_width=8 fallback=all-scalar\nRISC-V avg: 2 ms/frame", 3)
        with tempfile.TemporaryDirectory() as tmp, patch.object(bench, "execute", execute):
            result = bench.measure({"scalar": ["scalar"], "packet": ["packet"]}, Path(tmp), {}, 5)
        self.assertEqual([c[0] for c in calls[:6]], ["scalar", "packet", "scalar", "packet", "packet", "scalar"])
        self.assertTrue(all(c[-1] == "--no-video" for c in calls))
        self.assertEqual(len(result["samples"]["scalar"]), 5)
        self.assertIsNone(result["scalar_over_packet"])

    def test_missing_samples_are_unavailable(self):
        self.assertIsNone(bench.summary([{"ms": 2}, {"ms": None}])["median_ms"])

    def test_options_override_environment_and_preserve_positionals(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "options.cpp"
            binary = Path(tmp) / "options"
            source.write_text('''#include "test/benchmark_options.h"
int main(int argc, char** argv) {
    BenchmarkOptions options(argc, argv);
    std::printf("packet=%d bench=%d argc=%d last=%s\\n", std::getenv("SHADER_PACKET") != nullptr,
                options.bench, argc, argv[argc-1]);
}
''')
            subprocess.run(["g++", "-std=c++17", "-I", str(ROOT), str(source), "-o", str(binary)], check=True)
            env = os.environ | {"SHADER_PACKET": "0"}
            result = subprocess.check_output([str(binary), "vs", "fs", "name", "2", "64", "64", "--bench", "--packet-mode", "scalar"], env=env, text=True)
            self.assertIn("packet=0 bench=1 argc=7 last=64", result)
            self.assertIn("Video: disabled", result)
            result = subprocess.check_output([str(binary), "--video", "--no-video", "--packet-mode", "packet"], text=True)
            self.assertIn("Video: disabled", result)
            self.assertIn("packet=1", result)
            result = subprocess.run([str(binary), "--packet-mode", "invalid"], capture_output=True)
            self.assertEqual(result.returncode, 2)


def integration(build):
    spv = build / "spirv"
    jobs = {
        "fragment": [spv / "spirv_vulkan_host", spv / "quad.vert.spv", spv / "mandelbrot.frag.spv", "mandelbrot", 2, 64, 64],
        "mesh": [spv / "spirv_vulkan_mesh_host", spv / "mesh.vert.spv", spv / "mesh.frag.spv", "mesh", 2, "icosphere:1"],
        "texture": [spv / "spirv_vulkan_texture_host", spv / "quad.vert.spv", spv / "texture_test.frag.spv", "texture", 2, 64, 64],
        "volume": [spv / "spirv_vulkan_volume_host", spv / "quad.vert.spv", spv / "volume.frag.spv", "volume", 2, 32, 32],
        "volume_cs": [spv / "spirv_vulkan_volume_cs_host", spv / "volume.comp.spv", "volume_cs", 2, 32, 32],
        "life": [spv / "spirv_vulkan_life_host", spv / "life.comp.spv", 4, 32],
        "blur": [spv / "spirv_vulkan_compute_host", spv / "blur.comp.spv", "blur", 2],
    }
    with tempfile.TemporaryDirectory(prefix="benchmark-modes-") as tmp:
        tmp = Path(tmp)
        fake = tmp / "bin"
        fake.mkdir()
        sentinel = tmp / "ffmpeg-called"
        (fake / "ffmpeg").write_text(f'#!/bin/sh\ntouch "{sentinel}"\nexit 1\n')
        (fake / "ffmpeg").chmod(0o755)
        env = os.environ | {"PATH": str(fake) + ":" + os.environ["PATH"]}
        layer = Path("/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json")
        if layer.exists():
            env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
        for name, command in jobs.items():
            result = subprocess.run(list(map(str, command)), cwd=tmp, env=env, text=True, capture_output=True)
            output = result.stdout + result.stderr
            assert result.returncode == 0, (name, output)
            assert "Validation Error" not in output, (name, output)
            assert "Vulkan device avg:" in output, (name, output)
            sample = bench.parse_sample(output, True)
            assert sample["ms"] is not None and sample["ms"] > 0, (name, output)
            assert not sentinel.exists(), name
            assert not list((tmp / "result").rglob("*")), name
            print(f"PASS {name}: no video/images; timestamp {sample['ms']:.6f} ms", flush=True)
        command = list(map(str, jobs["fragment"]))
        result = subprocess.run(command + ["--bench"], cwd=tmp, env=env, text=True, capture_output=True)
        assert result.returncode == 0, result.stderr
        assert bench.parse_sample(result.stdout, True)["ms"] > 0, result.stdout
        assert not list((tmp / "result").rglob("*"))
        if shutil.which("ffmpeg"):
            result = subprocess.run(command + ["--video"], cwd=tmp, text=True, capture_output=True)
            assert result.returncode == 0, result.stderr
            assert (tmp / "result/mandelbrot.mp4").stat().st_size > 0
            print("PASS explicit --video produces MP4", flush=True)
        else:
            raise RuntimeError("ffmpeg missing; cannot verify explicit video mode")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Metrics)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)
    if args.build_dir:
        integration(args.build_dir.resolve())
