#!/bin/bash
#SBATCH -J first_try   #Job name
#SBATCH -A  ACF-UTK0011 #Write your project account associated to utia condo
#SBATCH -p short
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1  #--ntasks-per-node is used when we want to define the number of processes per node
#SBATCH --cpus-per-task=64
#SBATCH --time=03:00:00
#SBATCH --qos=short

export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK


module load openmpi/4.1.5-gcc 


###########   Run your parallel executable with srun   ###############
### srun -n "$2" --cpu-bind=cores ./hello_world  #-n is the total number of processes for the job



for file in "$@"; do
       ## srun  ./bin/"$file"  raw_text_input/1399.txt.utf-8.txt> output.txt
        srun  ./bin/"$file"  text_inputs/random_6400000.txt > output.txt
done
