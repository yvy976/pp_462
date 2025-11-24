import random
import sys

n = int(sys.argv[1])

words = ["one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"]

a = "abcdefghijklmnopqrstuvwxyz"
with open(f"random_{n}.txt", "w") as file:
    for _ in range(n):
        x = ""
        for i in range(5):   
            x += random.choice(a);  
#        file.write(random.choice(words))
        file.write(x)
        file.write("\n")


