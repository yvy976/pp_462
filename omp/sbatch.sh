#!/bin/bash
#SBATCH -J test   #Job name
#SBATCH -A  ACF-UTK0011  #Write your project account associated to utia condo
#SBATCH -p short
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1  #--ntasks is used when we want to define total number of processors
#SBATCH --time=03:00:00
#SBATCH --qos=short
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

#load gcc compiler module
module load gcc/10.2.0

"$@"
