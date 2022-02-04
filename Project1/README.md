Note that contrary to the instructions, the make command builds the proj1client and proj2client executables in the client and server folders, respectively.
This is because if both were in the root directory, one cannot tell if the file was actually transfered.
So to run the project, first execute make in the Project1 directory.
Then, to run the client, execute cd client; ./proj1client <host> <port> <file>
Likewise, to run the server, execute cd server; ./proj1server <port>



Performance Summary
|            | Cross-machine  | Local           |
|------------|----------------|-----------------|
| Small file | (47.16, 28.75) | (55.82, 8.61)   |
| Large file | (72.31, 43.98) | (857.63, 17.81) |

Note that the first number in the pair is the mean, and the second is the standard deviation.

We notice a few general trends.
First, the throughput of large files exceeds that of small files in either environment.
This is because a TCP connection is relatively expensive to set up, and so transfering a larger file amortizes this cost, lowering the ratio of time spent in overhead to time spent in transfer.
Also, TCP connection throughput tends to speed up as the transfer continues as a result of its "slow start", so a longer transfer yields greater time for the throughput to increase to its ceiling, and less percentage of total time spent during the slow start.

Second, the transfer throughput is much higher when transfering locally than across machines.
This is because we can just use the loopback connection and skip the network entirely.

Third, the standard deviation is much higher when transfering across computers than locally.
This is because, again, when transferring locally we can skip communication over the network.
When transferring across machines, however, we must get the network involved, which introduces much variability based on the current network traffic, routing paths taken, etc.
