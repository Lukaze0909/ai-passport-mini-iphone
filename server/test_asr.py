#!/usr/bin/env python3
"""Tests for the transcript repetition guard in asr.py (no ASR engine needed).

    python3 test_asr.py        (or: python3 -m pytest test_asr.py)
"""
import os, sys, unittest

os.environ["PASSPORT_ENV_FILE"] = os.devnull
os.environ["ASR_REPEAT_MIN"] = "4"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asr  # noqa: E402


class RepetitionGuardTest(unittest.TestCase):
    def test_drops_hallucinated_loops(self):
        for t in ("thank you " * 8,
                  "Okay. Okay. Okay. Okay. Okay. Okay. Okay.",
                  "谢谢观看" * 5,
                  "嗯" * 12,
                  "please like and subscribe, " * 6):
            self.assertTrue(asr.looks_pathological(t), t)

    def test_keeps_normal_speech(self):
        for t in ("no no no, I said the blue one",
                  "I think I think we should go",
                  "remind me to buy milk and eggs tomorrow morning",
                  "今天下午三点去拿快递，别忘了带钥匙",
                  "哈哈哈",
                  ""):
            self.assertFalse(asr.looks_pathological(t), t)

    def test_transcribe_none_engine(self):
        os.environ["PASSPORT_ASR"] = "none"
        try:
            self.assertEqual(asr.transcribe("/nonexistent.wav"), ("", {"engine": "none"}))
        finally:
            os.environ.pop("PASSPORT_ASR")

    def test_transcribe_reports_errors(self):
        os.environ["PASSPORT_ASR"] = "sensevoice"
        os.environ["SENSEVOICE_MODEL"] = "/nonexistent/model.onnx"
        try:
            text, info = asr.transcribe("/nonexistent.wav")
            self.assertEqual(text, "")
            self.assertIn("error", info)
        finally:
            os.environ.pop("PASSPORT_ASR"); os.environ.pop("SENSEVOICE_MODEL")


if __name__ == "__main__":
    unittest.main(verbosity=1)
