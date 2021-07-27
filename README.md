# Gromacs that can be accelerated at AMD GPU

## GPU acquirement
### Any AMD GPU supporting ROCm framework, especially fast at Instinct series like MI100 / MI50

## System acquirement

### capable linux system equipped with ROCm framework, centos 8.3 recommended

## installation guide
```
cd GROMACS-AMDGPU
cmake .. -DGMX_GPU_USE_AMD=on
make -j12
make check
make install
```

## estimate performance
```
MI100 = V100S 2X
MI100 = A100 1.3X
2*MI100 = MI100 1.2X
```

### external links
### initial Gromacs repo
#### https://github.com/gromacs/gromacs

### amd hipify docker image
#### https://www.amd.com/en/technologies/infinity-hub/gromacs
