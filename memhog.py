import numpy as np
import time
import sys

# this will throw an error if no argv[1]. Too bad!
arr = np.ndarray(shape=(1, 1024**3*int(float(sys.argv[1]))), dtype='c')
while (True):
    time.sleep(0.001)
    arr.fill('a')
