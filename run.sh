mkdir -p outputs_time3
rm outputs_time3/*
for i in 1 2 4 8 16 32 64; do
  n=$(($i * 100000))
  sbatch --cpus-per-task=$i -o "./outputs_time3/pp_$i.txt" sbatch.sh ./bin/cc ./text_inputs/random_$n.txt

done

