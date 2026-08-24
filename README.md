# Bedrock Desert WellFinder

 A Minecraft Bedrock Edition research project focused on finding rare physically clustered desert wells.

The primary focus is detecting 3-well and 4-well configurations based on the actual physical positions of the generated structures.

This project is focused on physical well clustering rather than simply checking whether wells exist in neighboring chunks.

---

## Overview

Bedrock Desert WellFinder is a research and development project for investigating desert well generation and finding rare combinations of multiple wells.

The project contains search programs, physical-cluster detection tools, validation programs, experiments, and previous research versions.
```
The main research target is:

3 physically clustered wells
            |
            v
4 physically clustered wells
            |
            v
Rare physical well configurations
```
---

## Main Goals

- Find physical 3-well clusters.
- Find physical 4-well clusters.
- Analyze the actual coordinates of generated wells.
- Detect physical relationships between multiple wells.
- Validate candidate seeds.
- Reduce false positives.
- Perform large-scale seed searching.
- Preserve useful research and validation results.

---

## Core Concept

A neighboring chunk does not necessarily mean that the generated structures are physically close.

For this reason, the project separates chunk-based relationships from physical structure relationships.
```
Chunk Location
      |
      v
Well Candidate
      |
      v
Actual Structure Position
      |
      v
Physical Distance
      |
      v
Cluster Analysis
      |
      +----------------+
      |                |
      v                v
   3-Well           4-Well
   Cluster          Cluster
```
The physical positions of the wells are therefore an important part of the detection process.

---

## Detection Pipeline

The general research pipeline is:

                         WORLD SEED
                             |
                             v
                      SEARCH REGION
                             |
                             v
                     CANDIDATE CHUNKS
                             |
                             v
                     BIOME VALIDATION
                             |
                             v
                    WELL GENERATION
                         CHECK
                             |
                             v
                  ACTUAL WELL POSITION
                             |
                             v
                  PHYSICAL DISTANCE
                         ANALYSIS
                             |
                             v
                    CLUSTER DETECTION
                       /           \
                      /             \
                     v               v
                3-WELL CLUSTER   4-WELL CLUSTER
                      \             /
                       \           /
                        v         v
                         VALIDATION
                             |
                             v
                      VERIFIED RESULT

Different research versions may implement individual stages differently.

---

### Physical 3-Well Finder

The physical 3-well research searches for three desert wells that satisfy the physical-cluster conditions being investigated.

The important distinction is that the search is not based only on chunk adjacency.

The actual world positions of the structures are analyzed to determine their physical relationship.
```
3-Well Research Process

Candidate wells
      |
      v
Calculate actual positions
      |
      v
Compare physical distances
      |
      v
Build physical relationship
      |
      v
Check cluster conditions
      |
      v
Validate candidate
```
---

### Physical 4-Well Finder

The physical 4-well research searches for four desert wells forming a physical cluster.

These configurations are also referred to as quadwell candidates.

The objective is to find rare seeds where four actual desert well structures satisfy the physical-cluster requirements.
```
4-Well Research Process

Candidate wells
      |
      v
Find physical relationships
      |
      v
Build connected component
      |
      v
Check 4-well configuration
      |
      v
Validate physical positions
      |
      v
Record candidate
```
---

## Baseline

The repository contains multiple versions of the WellFinder created during development.

A baseline is useful because it provides a known reference point when testing later changes.

### Baseline Principles

- Preserve known working implementations.
- Do not overwrite important research versions.
- Compare new changes against previous behavior.
- Keep regression tests for important discoveries.
- Separate experimental code from verified code.

### Research Version History

Several source files represent different stages of the research.

Examples include:
```
well_search.c
well_search_physical.c
well_search_quad.c

well_search_physical_finder.c
well_search_physical_finder_4d_base.c
well_search_physical_finder_4d_work.c

well_search_physical_finder_4e_step1_pass.c
well_search_physical_finder_4e_step2_pass.c
well_search_physical_finder_4e_step2_verified.c
well_search_physical_finder_4e_step3_base.c

well_search_physical_finder_4f_random_prod.c
well_search_physical_finder_4f_random_1b.c
well_search_physical_finder_4f_step4_verified.c
well_search_physical_finder_4f_step9f_verified.c
well_search_physical_finder_4f_step9g_pre_patch.c
well_search_physical_finder_4f_timer_test.c
```
These files represent research history and should not automatically be treated as interchangeable production versions.

---

## Tutorial

This section provides a basic workflow for building and testing the WellFinder.

### Requirements

Recommended environment:
```
- Linux
- Termux
- GCC or Clang
- pthread support
- Cubiomes
```
Clone the Repository
```
git clone https://github.com/jorosdavenmangadlao2-bot/bedrock-wellfinder.git
cd bedrock-wellfinder
```
### Build Cubiomes

Build the required library using the repository's build system:
```
make
```
### Compile a WellFinder

Example:
```
gcc -O3 -pthread well_search_physical_finder_4d_work.c \
-L. -lcubiomes -lm \
-o well_search_physical_finder_4d_work
```
Run the Program
```
./well_search_physical_finder_4d_work
```
Some research versions require command-line arguments.

Always inspect the selected source before running it because different versions may use different parameters.

For example:
```
grep -n "argc\|argv" well_search_physical_finder_4d_work.c
```
---

## Example Research Workflow
```
1. Select a WellFinder version
        |
        v
2. Build the required library
        |
        v
3. Compile the selected program
        |
        v
4. Configure the search
        |
        v
5. Search candidate chunks
        |
        v
6. Detect possible wells
        |
        v
7. Calculate physical positions
        |
        v
8. Analyze physical distances
        |
        v
9. Detect 3-well / 4-well clusters
        |
        v
10. Validate candidates
        |
        v
11. Record verified results
```
---

## Validation

A search result should not automatically be considered verified.

Candidate results should be checked using the project's validation and audit programs.

Important validation files include:
```
well_physical_audit.c
well_physical_scan.c
well_physical_validator.c

physical_test.c
physical_3well_test.c
physical_4well_test.c
physical_component_test.c
physical_component_bfs_test.c
physical_component_step6_test.c
physical_reporting_test.c
```
### Validation Goals

Validation can be used to examine:

- Actual well coordinates
- Physical distances
- Cluster membership
- Candidate correctness
- False positives
- Regression behavior
- Physical structure relationships

---

## Repository Structure

The repository contains several categories of research files.

### Core WellFinder Programs
```
well_search.c
well_search_physical.c
well_search_quad.c
well_search_physical_finder.c
```
### Physical Cluster Research
```
physical_3well_test.c
physical_4well_test.c
physical_component_test.c
physical_component_bfs_test.c
physical_component_step6_test.c
physical_reporting_test.c
```
### Physical Validation
```
well_physical_audit.c
well_physical_scan.c
well_physical_validator.c
```
### Y-Level Research
```
well_y_filter_test.c
well_y_finder.c
well_y_high_scan.c
well_y_physical_test.c
well_y_record_finder.c
well_y_rect.c
well_y_scan.c
well_y_single.c
well_y_windswept_test.c
``` 
### Research Data and Documentation
```
physical_5000.txt
positive_2well.txt
positive_2well_1_10000.txt
research_commands.txt
```
---

## Y-Level Research

Y-level research is an ongoing supporting research area.

The purpose is to investigate the vertical position of desert wells and the terrain conditions involved in their generation.

This work is separate from the primary physical-cluster objective.

The main focus of the repository remains:
```
Physical 3-Well Detection
          +
Physical 4-Well Detection
```
Y-level research should therefore be considered experimental and ongoing until the behavior has been sufficiently validated.

---

## Research Philosophy

This project follows a verification-first approach.

### Principles

1. Physical structure positions are more important than chunk adjacency alone.
2. Candidate discoveries should be independently validated.
3. Known-good source versions should be preserved.
4. Experimental versions should remain identifiable.
5. Regression tests should be preserved when useful.
6. Research claims should be supported by reproducible tests.
7. Generation behavior should not be considered verified without sufficient validation.

---

## Project Status

### Main Research

- [x] Bedrock desert well research
- [x] Physical 3-well research
- [x] Physical 4-well research
- [x] Physical cluster detection
- [x] Validation and audit tools
- [x] Large-scale search experiments

### Ongoing Research

- [ ] Y-level research
- [ ] Further physical-cluster validation
- [ ] Search optimization
- [ ] Production finder consolidation
- [ ] Additional regression testing
- [ ] Improved result reporting

---

## Future Development

### Planned development includes:

- Checkpoint and resume support
- Better result files
- Duplicate protection
- Search statistics
- Smart resume
- Adaptive threading
- Verified two-stage filtering
- Statistical analysis
- Spatial distribution analysis
- Distributed seed searching

---

## Cubiomes

This project uses and builds upon the Cubiomes library by Cubitect.

Cubiomes is distributed under the MIT License.

The applicable original copyright notice and license terms are preserved where required.

The WellFinder research in this repository focuses on Minecraft Bedrock desert well generation, physical structure detection, seed searching, and validation.

---

## License

This project is released under the MIT License.

See the "LICENSE" file for the complete license text.

---

## Disclaimer

This repository is an active research project.

Some programs are experimental, historical, or still under development.

Research results should be independently validated before being considered confirmed Minecraft generation behavior.

---

## Project Description

Bedrock Desert WellFinder is a Minecraft Bedrock Edition research project dedicated to finding rare physically clustered desert wells.

The primary objective is detecting 3-well and 4-well configurations using the actual physical positions of the generated structures.

The project combines seed searching, Bedrock generation research, physical structure analysis, validation, and large-scale search techniques.

The research is ongoing and the implementation will continue to evolve as new generation behavior is tested and verified.
