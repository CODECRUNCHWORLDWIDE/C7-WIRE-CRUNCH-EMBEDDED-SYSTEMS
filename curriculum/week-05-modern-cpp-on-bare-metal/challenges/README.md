# Week 5 — Challenges

Challenges are optional but recommended. They push past the exercise spec into territory that real production firmware spends time in.

| File                                                                                                       | What you build                                                                  | Time   |
|------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------|--------|
| [challenge-01-cpp-rewrite-of-uart-driver.md](./challenge-01-cpp-rewrite-of-uart-driver.md)                 | Re-implement Week 3's UART driver in C++20; produce the same Saleae trace; diff the `.elf` | 2 h    |

Challenges differ from exercises in three ways:
1. They are graded harder — production-grade is the bar, not "it works on the bench."
2. They include failure-injection requirements: you must demonstrate the failure case as well as the success case.
3. They are revisited in later weeks. Challenge 1 returns in Week 7 (UART under DMA) and Week 12 (debugging the UART rate).
