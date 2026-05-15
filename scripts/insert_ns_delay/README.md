# Inject latency with cycles level precision

Anyone interested in latency injection can read the code to get the details.

just:

```
sudo apt install msr-tools
sudo ./run.sh
```

I run it on Intel(R) Xeon(R) Gold 6448H 128-Core Processor with fixed frequency(2.4GHz). This injection method forms a stepped shape on this scatter plot since the overhead of `rdtsc` and `pause` is inevitable. And the minimum injection granularity is approximately 31.5ns.
