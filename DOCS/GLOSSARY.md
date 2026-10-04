# Glossary

Keep this open in a second window while reading the other documents.
Ordered roughly by when you meet each term, not alphabetically.

---

## The hardware

| Term | Meaning |
|---|---|
| **NAND flash** | The non-volatile memory chips that store the data in SSDs, pen drives and phones. |
| **Cell** | One transistor storing one or more bits, using electrons trapped on a floating gate. |
| **Floating gate** | The insulated piece of metal inside a cell where electrons are trapped. Insulation is why the data survives power-off. |
| **Program** | Writing. Pushes electrons onto the floating gate. Turns 1s into 0s. **Never the reverse.** |
| **Erase** | Pulls electrons off. Turns 0s back into 1s. Only works on a whole block. |
| **Page** | The unit of read and write. 4,096 bytes here. |
| **Block** | The unit of erase. 64 pages = 256 KiB here. |
| **Die / chip** | The whole piece of silicon. 128 blocks = 32 MiB here. |
| **Spare area / OOB** | Extra bytes attached to each page, beyond the user data. Used for ECC parity and firmware bookkeeping. |
| **P/E cycle** | One program/erase cycle. Blocks survive a limited number (3,000 here). |
| **Endurance** | How many P/E cycles a block survives before it must be retired. |
| **SLC / MLC / TLC / QLC** | 1/2/3/4 bits per cell. More bits = cheaper storage, far less endurance. |
| **Read disturb** | Reading a page slightly degrades other cells in the same block. Enough reads without an erase causes bit errors. |
| **ECC** | Error-Correcting Code. Hardware that detects and fixes bit errors. A CRC stands in for it here. |

## Addresses

| Term | Meaning |
|---|---|
| **LBA** | Logical Block Address. What the host operating system calls a location. |
| **LPN** | Logical Page Number. This project's name for the same idea — the address the host asks for. A *name*. |
| **PPN** | Physical Page Number. Where the data actually sits on the chip. A *place*. Changes on every write. |
| **PBN** | Physical Block Number. |
| **Mapping table / L2P** | Logical-to-Physical. The table converting LPNs to PPNs. The FTL's core data structure. |

## The firmware

| Term | Meaning |
|---|---|
| **Firmware** | Software running on a small dedicated device, usually without an OS, controlling hardware directly. |
| **Controller** | The small computer inside an SSD that runs the firmware. |
| **FTL** | Flash Translation Layer. The firmware layer that makes flash look like a disk to the host. **This project.** |
| **Host** | The computer the drive is plugged into. |
| **Out-of-place update** | Writing new data to a fresh page instead of overwriting, then repointing the mapping table. The FTL's central trick. |
| **Write frontier / active block** | The one block currently open for appending writes. |
| **Free pool** | The set of erased blocks available for allocation. |

## Page states

| Term | Meaning |
|---|---|
| **Free** | Erased, ready to be programmed. |
| **Valid** | Programmed, and the mapping table points at it. Live data. |
| **Invalid** | Programmed, but superseded. Dead. Unusable until its block is erased. |

## Garbage collection

| Term | Meaning |
|---|---|
| **Garbage collection (GC)** | Copying the valid pages out of a block so it can be erased and reused. |
| **Victim block** | The block GC has chosen to reclaim. |
| **Victim selection policy** | The rule for choosing. Greedy, cost-benefit, FIFO, random. |
| **Greedy** | Pick the block with the fewest valid pages. |
| **Cost-benefit** | Score `(1−u)·age / 2u`. Accounts for how cold a block is, not just how empty. |
| **Write amplification (WA)** | Physical writes ÷ host writes. **The headline metric.** WA = 3 means writing 1 GB puts 3 GB on the flash. |
| **Over-provisioning (OP)** | Capacity hidden from the host, used as GC breathing room. 25% here. |
| **Watermark** | The free-block level that triggers (low) or stops (high) GC. |
| **Hysteresis** | The gap between the two watermarks, which stops GC thrashing on and off. |
| **Preconditioning** | Filling the drive before measuring, so you measure steady state rather than the meaningless fresh-drive case. |
| **TRIM** | Host command saying "I'll never read these addresses again", letting GC skip that data. NVMe calls it Deallocate. |

## Mapping schemes

| Term | Meaning |
|---|---|
| **Page mapping** | One table entry per page. Most flexible, most RAM. What modern SSDs use. |
| **Block mapping** | One entry per block; page offsets fixed. 64× less RAM, terrible random writes. |
| **Hybrid mapping** | Block-mapped data blocks plus a small pool of page-mapped log blocks. |
| **Log block** | A block that temporarily absorbs updates for one data block, in any order. |
| **Merge** | Folding a log block back into a block-mapped data block. |
| **Switch merge** | The cheap case — the log block is already a complete valid data block, so just repoint. Zero copies. |
| **Partial merge** | Copy the missing tail from the data block into the log block, then switch. |
| **Full merge** | Allocate a third block and copy the newest version of every offset into it. Two erases. The expensive case. |
| **BAST** | Block-Associative Sector Translation. At most one log block per data block. What this project implements. |
| **FAST** | Fully-Associative Sector Translation. Any log block serves any data block. Fixes thrashing, costlier merges. |
| **Log block thrashing** | BAST's failure mode: a workload touching more logical blocks than there are log blocks forces a merge per write. |
| **Read-modify-write (RMW)** | Rebuilding a whole block to change one page. Block mapping's expensive path. |

## Wear

| Term | Meaning |
|---|---|
| **Wear levelling** | Spreading erases evenly so all blocks reach end-of-life together. |
| **Dynamic wear levelling** | Always allocate the least-erased *free* block. Free, but blind to cold data. |
| **Static wear levelling** | Deliberately relocate cold data off young blocks so they rejoin the pool. Costs writes, buys endurance. |
| **Erase-count spread** | max − min erase count across blocks. The metric wear levelling optimises. |
| **Cold data** | Data written once and rarely updated. Pins the block it sits on. |
| **Hot data** | Frequently overwritten data. |

## Reliability

| Term | Meaning |
|---|---|
| **Factory bad block** | A block marked unusable by the manufacturer before shipping. Must never be erased. |
| **Grown bad block** | A block that fails during normal use. |
| **Retire** | Permanently mark a block unusable — *after* rescuing its data. |
| **Fault injection** | Deliberately making the simulated chip fail operations, to test the recovery paths. |
| **Refresh** | Rewriting a block elsewhere to reset its read-disturb budget. |

## Power loss

| Term | Meaning |
|---|---|
| **Checkpoint** | A periodic snapshot of the FTL's state, written to a fixed metadata region. |
| **Journal** | A durable record of changes. Here the spare area *is* the journal. |
| **Self-describing media** | Every page carries its own LPN, so the mapping table can be rebuilt by scanning. |
| **Sequence number (seq)** | A globally increasing counter stamped on every page. Higher = newer. Resolves duplicates at recovery. |
| **Roll-forward** | Applying changes made after the last checkpoint. |
| **Torn write** | A multi-page write interrupted partway. Detected by writing the commit header last. |
| **Mount** | Bringing the FTL up over existing media — booting the drive. |
| **Atomic write** | Write to a temp name, then rename. A crash can't leave a half-written file. |

## Interfaces

| Term | Meaning |
|---|---|
| **NVMe** | The modern protocol for talking to SSDs over PCIe. Uses submission/completion queue pairs. |
| **PCIe** | The physical bus NVMe drives plug into. |
| **Submission / completion queue** | Ring buffers in host memory holding commands and their results. |
| **Doorbell** | A register the host writes to tell the drive new commands are queued. |

## C and build

| Term | Meaning |
|---|---|
| **Header (`.h`)** | Declares what exists — function signatures, structs. |
| **Source (`.c`)** | The implementation. |
| **Static allocation** | Fixed-size arrays sized at compile time. No `malloc`. Required on real controllers. |
| **`.bss`** | The part of the binary holding zero-initialised static data. Its size tells you the RAM budget. |
| **Invariant** | A condition that must always hold. `ftl_check_invariants()` verifies seven of them. |
| **`-Werror`** | Compiler flag making every warning a build failure. |
| **Valgrind / AddressSanitizer** | Tools that catch memory errors at runtime. `make memcheck`, `make asan`. |
