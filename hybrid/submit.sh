#!/bin/bash
OUT_DIR=output
LOG_DIR=logs

FILES=$(for i in {1..40}; do echo ../omp/text_inputs/random_6400000.txt; done)

for p in 1 16; do
	t=4
	NAME=${p}p_${t}t
	sbatch --ntasks-per-node=$p --cpus-per-task=4 -o $LOG_DIR/$NAME.log ./sbatch.sh ./bin/wc $OUT_DIR/$NAME.out $FILES
done
