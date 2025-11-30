#!/bin/bash
FILES=$(for i in {1..40}; do echo ../omp/text_inputs/random_6400000.txt; done)

sbatch --ntasks-per-node=1 --cpus-per-task=4 -o output_p1.txt ./sbatch.sh ./bin/wc $FILES
sbatch --ntasks-per-node=16 --cpus-per-task=4 -o output_p16.txt ./sbatch.sh ./bin/wc $FILES
