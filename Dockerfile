FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake make python3 python3-pip && rm -rf /var/lib/apt/lists/*
WORKDIR /artifact
COPY . .
RUN python3 run.py build
CMD ["bash", "scripts/quickstart.sh"]
