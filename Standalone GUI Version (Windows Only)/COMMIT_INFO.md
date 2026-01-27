# Git Commit Info for GPU-PCIe-Test v2.5

## Commit Message

```
v2.5: Add TB5/PCIe 6.0, fix averaging modes, accumulating results

Major changes:
- Added PCIe 6.0 x16 (126 GB/s) and Thunderbolt 5 (10 GB/s) to interface standards
- Results now accumulate across multiple benchmark runs (don't clear between runs)
- Added "Clear Charts" button to manually clear accumulated results
- Added "Reset Settings" button to restore defaults without clearing charts
- Switching between Average/Individual modes automatically clears results
- Individual mode now uses best (max) values for interface comparison
- Graphs show "Best" label instead of "Max" in individual mode
- Compare to Standards shows mode indicator (Avg/Best)
- Bandwidth slider now allows full safe size based on GPU VRAM (removed 1024 MB cap)
- Start Benchmark button now available after completion (to run additional tests)
- Fixed graphs to show oldest runs at top when using individual mode

UI improvements:
- Tooltip on bandwidth slider shows max safe size
- Tooltip on Average Runs checkbox warns about clearing results
- Mode-specific labels throughout (Avg vs Best)
- Better state management for results windows
```

## Git Commands

```bash
# Navigate to your repository
cd GPU-PCIe-Test

# Stage the changed files
git add main_gui.cpp README.md

# Commit with the message
git commit -m "v2.5: Add TB5/PCIe 6.0, fix averaging modes, accumulating results

- Added PCIe 6.0 x16 and Thunderbolt 5 to interface standards
- Results accumulate across multiple benchmark runs
- Added Clear Charts and Reset Settings buttons
- Switching Average/Individual modes clears results
- Individual mode uses best (max) values for comparison
- Bandwidth slider allows full VRAM-based safe size
- Start Benchmark available after completion for additional runs
- Graphs show oldest runs at top in individual mode
- Updated README with new features and changelog"

# Push to GitHub
git push origin main
```

## Files Changed

1. **main_gui.cpp** - Main source file with all changes
2. **README.md** - Updated documentation with features, usage, and changelog

## Summary of Interface Standards (v2.5)

| Interface | Bandwidth |
|-----------|-----------|
| PCIe 3.0 x4 | 3.94 GB/s |
| PCIe 3.0 x8 | 7.88 GB/s |
| PCIe 3.0 x16 | 15.75 GB/s |
| PCIe 4.0 x4 | 7.88 GB/s |
| PCIe 4.0 x8 | 15.75 GB/s |
| PCIe 4.0 x16 | 31.51 GB/s |
| PCIe 5.0 x8 | 31.51 GB/s |
| PCIe 5.0 x16 | 63.02 GB/s |
| **PCIe 6.0 x16** | **126.03 GB/s** *(NEW)* |
| OCuLink 1.0 | 3.94 GB/s |
| OCuLink 2.0 | 7.88 GB/s |
| Thunderbolt 3 | 2.50 GB/s |
| Thunderbolt 4 | 3.00 GB/s |
| **Thunderbolt 5** | **10.00 GB/s** *(NEW)* |
| USB4 40Gbps | 4.00 GB/s |
| USB4 80Gbps | 8.00 GB/s |
