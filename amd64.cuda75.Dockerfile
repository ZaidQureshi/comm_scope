FROM c3sr/scope:amd64-cuda75-latest

# Comm|Scope wants NUMA
RUN apt-get update && apt-get install -y --no-install-recommends --no-install-suggests \
    libnuma-dev \
    && rm -rf /var/lib/apt/lists/*

# Add current Comm|Scope source
RUN rm -rf scopes/comm_scope
ADD . scopes/comm_scope

# Rebuild binary
RUN cd build \
  && cmake -DENABLE_COMM=1 .. \
  && cmake -DENABLE_EXAMPLE=0 .. \
  && make