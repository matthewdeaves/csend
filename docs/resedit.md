# 🎨 Working with ResEdit Resources

---

## 🎯 Overview

This guide explains how to work with ResEdit-created resources (`.rsrc` files) and convert them for use with the Retro68 development toolchain. The key challenge is handling Classic Mac resource forks, which don't survive standard file transfers.

---

## 🖼️ Creating Resources with ResEdit

**ResEdit** is a visual resource editor that was commonly used to design user interface elements for Classic Macintosh applications.

### What You Can Create:
- 🪟 **Windows & Dialogs** - Layout and positioning
- 📋 **Menus** - Menu bars and dropdown items  
- 🎛️ **Controls** - Buttons, checkboxes, text fields
- 🎨 **Icons & Graphics** - Visual elements
- 📝 **String Resources** - Localized text

**Output**: ResEdit saves these designed elements into a binary **resource file** (typically `.rsrc` extension).

---

## 📁 Understanding Resource Forks

### ⚠️ The Resource Fork Problem

**Classic Mac File Structure:**
Classic Macintosh files have two parts:
- **Data Fork** - Traditional file content
- **Resource Fork** - UI elements, icons, structured data

**The Issue:**
- ResEdit stores visual elements in the **resource fork**
- Modern file systems (Linux ext4, Windows NTFS) **don't support resource forks**
- When copying `.rsrc` files from Mac VM → Ubuntu host, **only the data fork survives**
- Result: **All your UI design is lost!** 😱

**Solutions**: 
1. Convert binary resources to text format before transferring (recommended)
2. Use SheepShaver's `.finf` folders to preserve resource fork metadata

---

## 🔧 The Solution: DeRez Conversion

### Why DeRez?

**`DeRez`** converts binary resource files into text-based representations:
- ✅ **Preserves all resource data** in human-readable format
- ✅ **Text files transfer safely** between systems
- ✅ **Compatible with Retro68** build process
- ✅ **Version control friendly** (can diff changes)

### Conversion Flow

```mermaid
graph LR
    A[ResEdit Design] --> B[.rsrc Binary]
    B --> C[DeRez Tool]
    C --> D[.r Text File]
    D --> E[Transfer to Ubuntu]
    E --> F[Retro68 Build]
```

---

## 💻 Using DeRez within MPW

### Prerequisites
- ✅ MPW (Macintosh Programmer's Workshop) installed on Mac VM
- ✅ Your `.rsrc` file created with ResEdit

### Step-by-Step Process

#### 1. Launch MPW Shell
- Open **MPW Shell** application on your Mac VM
- You'll see a window titled "Worksheet"

#### 2. Enter DeRez Command
```mpw
DeRez "Macintosh HD:Path:To:YourProject:csend.rsrc" > "Macintosh HD:Path:To:YourProject:csend.r"
```

> **📝 Note**: Replace the path with your actual project location using Classic Mac path syntax (volume:folder:file)

#### 3. Execute the Command
- Place text cursor anywhere on the DeRez command line
- Press **Enter key on numeric keypad** (⌅) 
- ⚠️ **Important**: Use numeric keypad Enter, NOT the main Return key!

#### 4. Verify Success
- Check that `csend.r` file was created
- The file should contain human-readable resource definitions

### Example Commands

**Common DeRez Usage Patterns:**

```mpw
# Basic conversion
DeRez "MyProject:resources.rsrc" > "MyProject:resources.r"

# With specific resource types
DeRez "MyProject:app.rsrc" -only DLOG,DITL > "MyProject:dialogs.r"

# Including definitions
DeRez "MyProject:app.rsrc" -i "MPW:Interfaces:RIncludes:" > "MyProject:app.r"
```

---

## 📤 Transferring and Using the .r File

### Transfer Process

1. **Copy from Mac VM** - Transfer the `.r` text file to your Ubuntu host
2. **Place in Project** - Put it in your CSend project directory
3. **Update Makefile** - Ensure `Makefile.retro68` references the `.r` file

### Retro68 Integration

The Retro68 toolchain includes `Rez` (different from `DeRez`) which:
- ✅ Compiles `.r` text files back to binary resources
- ✅ Links resources into your final application
- ✅ Integrates with the build process

### Build Process Flow

```bash
# Your Makefile.retro68 should include:
Rez -i "$(RINCLUDES)" csend.r -o csend.rsrc
# Then link the resources into the final application
```

---

## 🚀 Quick Reference

### Command Summary

| Task | Command | Notes |
|------|---------|-------|
| **Convert .rsrc to .r** | `DeRez "path:file.rsrc" > "path:file.r"` | Use numeric keypad Enter |
| **Execute in MPW** | Position cursor on line + ⌅ | Not regular Return key |
| **Check conversion** | Open `.r` file in text editor | Should be human-readable |

### File Extensions

| Extension | Type | Description |
|-----------|------|-------------|
| `.rsrc` | Binary | ResEdit output (has resource fork) |
| `.r` | Text | DeRez output (Rez source code) |
| `.rsrc` | Binary | Rez output (compiled resources) |

---

## 🗂️ SheepShaver .finf Folders

### What are .finf Folders?

When using **SheepShaver** (Mac OS 9 emulator), the emulator creates special `.finf` folders to preserve Classic Mac file metadata:

- **Purpose**: Maintain resource fork and data fork information
- **Location**: Created alongside directories containing Mac files
- **Contents**: Binary metadata files that preserve Classic Mac file structure
- **Automatic**: Generated by SheepShaver when files are modified

### How They Work

```
Project Directory/
├── csend.rsrc          # Your resource file (data fork only on Linux)
├── .finf/              # SheepShaver metadata folder
│   └── csend.rsrc      # Resource fork metadata (32 bytes)
└── other_files...
```

### Benefits and Limitations

**✅ Benefits:**
- Automatically preserves resource fork data
- No manual conversion required
- Maintains original Mac file structure
- Works with any Mac file type

**⚠️ Limitations:**
- Only works with SheepShaver emulator
- `.finf` folders are emulator-specific
- Not portable across different Mac emulators
- Binary format (not version control friendly)

### Relationship to DeRez

**.finf folders and DeRez serve different purposes:**

- **`.finf` folders**: Automatically preserve resource fork data when transferring from SheepShaver
- **DeRez conversion**: Still needed to create version-controllable text files from the preserved binary resources

**Typical workflow with SheepShaver:**
1. Edit resources in ResEdit within SheepShaver
2. SheepShaver creates `.finf` folders to preserve resource forks
3. Transfer files to host system (resource forks preserved via `.finf`)
4. Use DeRez to convert binary `.rsrc` to text `.r` files for version control
5. Build with Retro68 using the `.r` files

---

## ⚠️ Common Issues

### 🐛 Troubleshooting

**Problem: Command doesn't execute**
- **Solution**: Use numeric keypad Enter (⌅), not Return key

**Problem: Path not found**
- **Solution**: Use Classic Mac path syntax with colons, not forward slashes
- **Example**: `"Macintosh HD:Users:name:project:file.rsrc"`

**Problem: Empty .r file**
- **Solution**: Check that original .rsrc has resource fork data
- **Test**: Open .rsrc in ResEdit to verify resources exist

**Problem: Build fails with .r file**
- **Solution**: Ensure Makefile includes proper Rez compilation step
- **Check**: Verify RIncludes path is correct

**Problem: .finf folders cluttering repository**
- **Solution**: Add `.finf` to `.gitignore` if using version control
- **Note**: You still need DeRez conversion for version-controllable resource files

**Problem: Resource fork lost despite .finf folder**
- **Solution**: Ensure you're using SheepShaver (not other emulators)
- **Check**: Verify .finf folder contains metadata files for your resources

---
