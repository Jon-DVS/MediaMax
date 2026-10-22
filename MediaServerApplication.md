# Media Server Application
We're building a production-grade, simple and straight-forward Media Server Application for Windows and Linux (Ubuntu 24+). This is professional-grade software that needs to be stable, fully-tested, and real-world deployable. The project name is MediaMax.

## Dev Workflow
- Ask questions only when absolutely necessary - work autonomously
- IMRORTANT! DO NOT look outside of working directory or use other code on my computer as reference
- Testing is of the utmost importance - use lots of unit tests (any framework, simple is good), run automated testing
- Do things properly, this is production-grade and not a hack project
- Create AGENTS.md and relevant skills to support development, and contain concrete development guidelines
- Create git repository, commit and push to [GitHub repo](https://github.com/Jon-DVS/MediaMax). IMPORTANT: do a code review before committing, and make sure all changes comply with code style, production-grade quality standards, and have been properly tested/have unit tests that have passed where necessary

## Tech stack
- C++ and CMake for core engine
- [GLFW](https://github.com/glfw/glfw) and [nvrhi](https://github.com/NVIDIA-RTX/NVRHI), using Vulkan primarily on all platforms
- [glm](https://github.com/g-truc/glm) for math
- [miniaudio](https://github.com/mackron/miniaudio) for audio

## Basic Architecture
- Static library for core engine, executable for playback engine and the control interface
- The control interface should allow that management of multiple systems running the playback engine executible
- The program should support multiple video playback, timelines,  multiple display output mangement, warping and blending of those outputs

## Similar Existing Software
Use existing media server software to inspire features and functionallity
Example Software:
- [Resolume](https://resolume.com/)
- [WATCHOUT](https://www.dataton.com/watchout)
- [PIXERA](https://pixera.one/en/)
