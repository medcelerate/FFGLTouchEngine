# FFGLTouchEngine For Resolume

Simple FFGL plugin that allows loading touchdesigner components (tox) into programs like resolume. 

### Notice
Nightly builds will be published to the releases page. [Download Here](https://github.com/medcelerate/FFGLTouchEngine/releases/latest)

This plugin is provided AS-IS from this repository, bug fixes and feature requests will be serviced best based on the time of the developers. If you intend to use this in production, please fill out the form below to discuss with the developers support options to ensure stability for live. 

[Support Form](https://forms.gle/QNSKGjdMsX1ptbvh7)

---

[If you like this tool please consider support my other venture and there will be many more tools to come.](https://www.kickstarter.com/projects/cvalt/help-build-new-york-citys-newest-zero-proof-cocktail-bar?ref=fhrd79)

Thank you to  ([@yannicks-png](https://github.com/yannicksengstock)) for the support and guidance.

[Download Latest Release Here](https://github.com/medcelerate/FFGLTouchEngine/releases)

## Important!

### You need at least a touchdesigner commercial license for this to work!

### Current Features
- [x] Support Tox as Generative FFGL
- [x] Support Exposed Parameters From Tox
- [x] Support Texture Input
- [ ] Dynamic allocation of touchengine processes
- [ ] Drag & Drop of Tox Into Resolume
- [ ] MacOS Support


### Guides

**Textures**

To output video from TouchDesigner you need to create an out TOP and set the name to `output`, make sure to hit `yes to all`. The same goes for inputs. If using the FX version make sure to create an in top named `input`.

**Parameters**

Due to FFGL limits, you can have at most 30 of each type of parameter. If you use more it could at the moment cause undefined behavior.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/Q5Q6YUGIA)
