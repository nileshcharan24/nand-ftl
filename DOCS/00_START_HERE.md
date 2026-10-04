# START HERE

You built this project. You are new to embedded and storage. This folder exists
to take you from **zero knowledge** to **explaining the whole thing confidently
in an interview**.

Read the documents in the order below. Do not skip. Each one assumes only what
the previous ones taught you.

---

## First, the 60-second version

Your phone, your laptop SSD, your pen drive — none of them store data on
spinning discs any more. They store it in **flash memory chips**.

Flash memory is fast and has no moving parts. But it has some deeply awkward
physical rules, and because of those rules **you cannot simply write data to it
the way you write to RAM**.

So every SSD and every pen drive contains a tiny computer — a **controller** —
running a program called **firmware**. The most important job of that firmware
is to hide flash's awkwardness from your operating system, so Windows can happily
say "write these 4096 bytes to block number 12345" and not care that flash
cannot actually do that.

The part of the firmware that performs this illusion is called the
**Flash Translation Layer**, or **FTL**.

**This project is an FTL, written from scratch in C, running against a fake NAND
flash chip that is also written from scratch in C.** Because the flash chip is
simulated, you can measure things you could never measure on real hardware, and
you can deliberately break things (make blocks fail, cut the power mid-write) to
prove the firmware survives.

That is the whole project. Everything else is detail.

---

## What "FTL" stands for, and why the name

| Word | Meaning here |
|---|---|
| **Flash** | The memory chips (NAND flash) that actually hold the bits. |
| **Translation** | Converting the address the host asks for into the address where the data really lives. |
| **Layer** | A slab of software sitting between the host and the chip. |

So: *the software layer that translates addresses for flash memory.*

---

## The study flow

Roughly 3–4 hours to read properly, with a pen.

### Stage 1 — Understand the problem (read these three, in order)

| # | Document | What you will know afterwards | Time |
|---|---|---|---|
| 1 | **[01_WHY_THIS_EXISTS.md](01_WHY_THIS_EXISTS.md)** | What an SSD is made of, what firmware is, and why this project needed to exist at all. | 15 min |
| 2 | **[02_HOW_FLASH_WORKS.md](02_HOW_FLASH_WORKS.md)** | How NAND flash physically works and its three awkward rules. **This is the most important document in the folder.** | 30 min |
| 3 | **[03_WHAT_AN_FTL_DOES.md](03_WHAT_AN_FTL_DOES.md)** | The central trick — out-of-place updates and the mapping table. | 25 min |

> After Stage 1, stop and try to explain to yourself, out loud, why you cannot
> overwrite a byte on a pen drive. If you cannot, re-read document 02.

### Stage 2 — Understand the consequences

Everything here exists *because* of the rules in document 02. Nothing is
arbitrary.

| # | Document | What you will know afterwards | Time |
|---|---|---|---|
| 4 | **[04_GARBAGE_COLLECTION.md](04_GARBAGE_COLLECTION.md)** | Why the drive has to tidy up after itself, and **write amplification** — the single most important number in storage firmware. | 30 min |
| 5 | **[05_WEAR_LEVELLING.md](05_WEAR_LEVELLING.md)** | Why flash wears out, and how firmware makes all the chips die at the same time instead of a few dying early. | 20 min |
| 6 | **[06_BAD_BLOCKS_AND_POWER_LOSS.md](06_BAD_BLOCKS_AND_POWER_LOSS.md)** | What happens when the chip fails, and how a drive survives having its power cable yanked out. | 25 min |

### Stage 3 — Understand your own code

| # | Document | What you will know afterwards | Time |
|---|---|---|---|
| 7 | **[07_REPO_TOUR.md](07_REPO_TOUR.md)** | What every single file and folder in the project is for. | 15 min |
| 8 | **[08_CODE_WALKTHROUGH.md](08_CODE_WALKTHROUGH.md)** | One write request followed line by line from the host down to the flash chip. | 40 min |

### Stage 4 — Be ready to be questioned

| # | Document | What you will know afterwards | Time |
|---|---|---|---|
| 9 | **[09_RESULTS.md](09_RESULTS.md)** | The actual measured numbers this project produced, what they mean, and how to reproduce them. | 20 min |
| 10 | **[10_INTERVIEW_QA.md](10_INTERVIEW_QA.md)** | Answers to the questions you will be asked, plus an honest list of the project's weaknesses. | 30 min |

**[GLOSSARY.md](GLOSSARY.md)** — every term in one line each. Keep it open in a
second window while you read everything else.

---

## The one analogy that carries the whole project

You will meet this in document 02 and it recurs everywhere. Get comfortable with
it now:

> **Flash memory is a notebook written in permanent ink.**
>
> - You can write on a **line** once. You cannot rub out one line.
> - The only eraser works on a whole **sheet** at a time, wiping every line on it.
> - Sheets get damaged a little each time you erase them. After a few thousand
>   erases a sheet falls apart and must be thrown away.
>
> To "change" something you already wrote, you write the new version on a fresh
> blank line somewhere else, and keep an **index** at the front of the notebook
> saying where the current version of everything lives.

Every single feature in this project falls out of that picture:

| In the notebook | In flash | Document |
|---|---|---|
| A line | A **page** (4096 bytes) | 02 |
| A sheet | A **block** (64 pages) | 02 |
| The index at the front | The **mapping table** | 03 |
| Writing a new version elsewhere | **Out-of-place update** | 03 |
| Recopying good lines so you can erase a sheet | **Garbage collection** | 04 |
| Ending up writing far more than you meant to | **Write amplification** | 04 |
| Rotating which sheets you use so none wears out first | **Wear levelling** | 05 |
| Sheets that arrive torn, or tear while in use | **Bad blocks** | 06 |
| Writing the chapter number in the margin of every line | The **spare area**, which is how the drive rebuilds its index after a power cut | 06 |

---

## What you should be able to say at the end

If you can deliver this paragraph without notes, the docs have done their job:

> "NAND flash can only be written one page at a time, can only be erased one
> whole block at a time, and a page can't be rewritten until its entire block is
> erased. So firmware never overwrites in place — it writes the new data to a
> fresh page and updates a table that maps logical addresses to physical ones.
> That leaves dead pages behind, so a garbage collector eventually has to copy
> the surviving pages out of a block and erase it. Those copies are extra writes
> the host never asked for, which is write amplification — I measured 2.7 for
> page-level mapping under random writes, against 64 for block-level mapping.
> Erasing wears the block out, so I also had to level the wear across blocks, and
> handle blocks that fail outright, and make the whole thing survive power loss."

---

## A note on how to read this

You are not trying to memorise. You are trying to reach the point where each
mechanism feels **inevitable** — where, if someone described the problem, you
would invent the solution yourself.

Every document therefore follows the same shape:

1. **The problem**, in plain language, with the notebook analogy.
2. **Why the obvious solution doesn't work.**
3. **What this project actually does**, with the real file and function names.
4. **The measured consequence**, with real numbers from `09_RESULTS.md`.
5. **What an interviewer will ask about it.**
