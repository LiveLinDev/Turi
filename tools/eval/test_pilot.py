"""Pruebas del protocolo que no requieren inferencia."""
import unittest

from pilot import TASKS, extract, prompt, source_for


class Harness(unittest.TestCase):
    def test_fence_policy(self):
        self.assertEqual(extract("```turi\nfn x() {}\n```"), "fn x() {}")
        self.assertEqual(extract("texto\n```turi\nfn x() {}\n```"), "texto\n```turi\nfn x() {}\n```")

    def test_cases_are_disjoint(self):
        for t in TASKS:
            self.assertGreaterEqual(len(t.hidden), 2)
            self.assertFalse(set(t.hidden) & set(t.public))
            for assertion in t.hidden:
                self.assertNotIn(assertion, prompt(t))
                self.assertNotIn(assertion, source_for(t, "", t.public))

    def test_signature_and_oracle_are_sent(self):
        for t in TASKS:
            self.assertIn(t.signature, prompt(t))
            self.assertIn(t.public[0], prompt(t))
            self.assertNotIn(t.solution, prompt(t))


if __name__ == "__main__":
    unittest.main()
