# AI Models Directory

Place ONNX model files here:

## Required Models

1. **component_detector.onnx** — YOLOv8 or RT-DETR model for component detection
   - Input: 640x640 RGB image
   - Output: bbox + class + confidence
   - Classes: resistor, capacitor, IC, connector, diode, inductor, etc.

2. **solder_inspector.onnx** — Solder joint quality classifier  
   - Input: 224x224 crop of individual solder joint
   - Output: 6 classes (good, insufficient, excess, bridge, cold, missing)

3. **ocr_model.onnx** — CRNN-based text recognition model (optional)
   - Input: Grayscale text ROI
   - Output: Character sequence

## Class Name Files

Each model should have a companion `.txt` file with class names (one per line):
- `component_detector.txt`
- `solder_inspector.txt`

## How to Generate Models

```bash
# Install ultralytics
pip install ultralytics

# Train YOLOv8 on PCB component dataset
yolo detect train data=pcb_components.yaml model=yolov8n.pt epochs=100

# Export to ONNX
yolo export model=best.pt format=onnx opset=17 simplify=True dynamic=False imgsz=640
```

## Pre-trained Models

Check the project releases page for pre-trained model downloads.
