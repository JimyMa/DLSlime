# Slime Transfer Engine

## Benchmark

- in H800 with NIC ("mlx5_bond_0"), RoCE v2.


```
Batch size        : 160
Block size        : 16384
Total trips       : 51263
Total transferred : 128157 MiB
Duration          : 10.0001 seconds
Throughput        : 12815.6 MiB/s
```

```
Batch size        : 160
Block size        : 32768
Total trips       : 37095
Total transferred : 185475 MiB
Duration          : 10 seconds
Throughput        : 18547.5 MiB/s
```

```
Batch size        : 160
Block size        : 65536
Total trips       : 20892
Total transferred : 208920 MiB
Duration          : 10.0003 seconds
Throughput        : 20891.3 MiB/s
```

```
Batch size        : 160
Block size        : 128000
Total trips       : 11409
Total transferred : 222832 MiB
Duration          : 10.0007 seconds
Throughput        : 22281.7 MiB/s
```

```
Batch size        : 160
Block size        : 256000
Total trips       : 5831
Total transferred : 227773 MiB
Duration          : 10.0005 seconds
Throughput        : 22776.2 MiB/s
```

```
Batch size        : 160
Block size        : 156000
Total trips       : 9446
Total transferred : 224849 MiB
Duration          : 10.0001 seconds
Throughput        : 22484.7 MiB/s
```

```
Batch size        : 160
Block size        : 512000
Total trips       : 2954
Total transferred : 230781 MiB
Duration          : 10.0011 seconds
Throughput        : 23075.5 MiB/s
```

```
Batch size        : 160
Block size        : 1024000
Total trips       : 1486
Total transferred : 232187 MiB
Duration          : 10.0002 seconds
Throughput        : 23218.4 MiB/s
```

```
Batch size        : 160
Block size        : 2048000
Total trips       : 742
Total transferred : 231875 MiB
Duration          : 10.0034 seconds
Throughput        : 23179.7 MiB/s
```

