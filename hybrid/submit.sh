#!/bin/bash
#
sbatch --ntasks-per-node=2 --cpus-per-task=8 -o output.txt ./sbatch.sh ./bin/wc ../omp/text_inputs/random_6400000.txt
