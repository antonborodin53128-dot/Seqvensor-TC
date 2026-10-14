# Rule: minimum latency
All audio code (built-in plugins, processing, monitoring) must add as little latency as possible.
First research and compare the lowest-latency options, only then implement. Zero lookahead / no FFT blocks / no oversampling
unless unavoidable; report unavoidable latency through setLatencySamples. Plugins are built one at a time.
