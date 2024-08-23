# FFGLTouchEngine For Resolume

Simple FFGL plugin that allows loading touchdesigner components (tox) into programs like resolume. 

### Notice
Nightly builds will be published to the releases page. [Download Here](https://github.com/medcelerate/FFGLTouchEngine/releases/latest)

This plugin is provided AS-IS from this repository, bug fixes and feature requests will be serviced best based on the time of the developers. If you intend to use this in production, please fill out the form below to discuss with the developers support options to ensure stability for live.

[Production Use Support Form](https://forms.gle/QNSKGjdMsX1ptbvh7)

### Download

[Download Latest Release Here](https://github.com/medcelerate/FFGLTouchEngine/releases)

## Important!

### Any of the following TouchDesigner licenses are need for this to work.
- Educational
- Commercial
- Pro

---

### Tests

- Currently known to work with TouchDesigner 2023.
- There is an issue with earlier versions of 2023 that cause Int menus not to appear.

---


### Current Features
- [x] Support Tox as Generative FFGL
- [x] Support Exposed Parameters From Tox
- [x] Support Texture Input
- [ ] Dynamic allocation of touchengine processes
- [ ] Drag & Drop of Tox Into Resolume
- [ ] MacOS Support

---

### Submitting Issues
Please file issues here on github.
- Attach a tox that has issues.
- Describe steps to reproduce.
- Describe the issue you are seeing.
- Attach screenshot of license.
- Describe OS and environment, i.e windows 11, Nvidia GPU etc.

---

### Guides

**Textures**

To output video from TouchDesigner you need to create an out TOP and set the name to `out1`, make sure to hit `yes to all`. The same goes for inputs. If using the FX version make sure to create an in top named `in1`.

If you are using a source make sure to set the resolution in TouchDesigner or expose it as a parameter.

8, 16, and 32 bit textures out of TouchDesigner are supported, however they will be downsampled to the max resolume supports (16 bit).

**Parameters**

Due to FFGL limits, you can have at most 30 of each type of parameter. If you use more it could at the moment cause undefined behavior.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/Q5Q6YUGIA)


Thank you to  ([@yannicks-png](https://github.com/yannicksengstock)) for the support and guidance.
