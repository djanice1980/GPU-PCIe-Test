v2.7: Improved APU and eGPU detection

## Integrated GPU (APU) Detection
- APUs no longer report fake PCIe speeds
- Proper UMA (Unified Memory Architecture) reporting
- Shows memory path, fabric type (AMD Infinity Fabric / Intel Ring Bus)
- Compares bandwidth against DDR specs instead of PCIe

## eGPU / External GPU Detection via Device Tree
- Hardware-based detection instead of bandwidth inference
- Detects Intel Thunderbolt/USB4 PCIe switches (Goshen Ridge, Titan Ridge, etc.)
- Detects AMD USB4 controllers (Ryzen 7000+)
- Detects ASMedia USB4 controllers
- Detects USB4 Routers by device description
- Falls back to bandwidth detection if hardware detection fails

## New Device IDs Supported
- Intel Goshen Ridge USB4/TB4 PCIe switches (0x5780-0x578F)
- Intel Titan Ridge, Alpine Ridge, Maple Ridge switches
- AMD USB4: Pink Sardine, Rembrandt, Phoenix, Hawk Point
- ASMedia USB4: ASM2364, ASM4242

## Output Improvements
- "confirmed via device tree" vs "detected via bandwidth"
- Cleaner eGPU status messages
- PCIe link labeled as "GPU to Enclosure" for eGPUs
- Debug logging option for troubleshooting detection

## Example Output (eGPU):
Connection: USB4 / Thunderbolt (PCIe Tunnel) (confirmed via device tree)
eGPU Status: External GPU detected

## Example Output (APU):
Memory Path: Integrated GPU (Shared Memory)
Fabric: AMD Infinity Fabric
Memory Type: DDR5
PCIe: Not Applicable (on-die GPU)
