#!/bin/bash
#SBATCH -A ACF-UTK0011
#SBATCH -p short
#SBATCH --nodes=1
#SBATCH --qos=short
#SBATCH --time 0-00:10:00

export OMP_NUM_THREADS=${SLURM_CPUS_PER_TASK}

###########   Run your parallel executable with srun   ###############
echo RUNNING: ${@}
srun --cpu-bind=cores ${@}
