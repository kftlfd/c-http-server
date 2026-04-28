FROM ubuntu:26.04

RUN apt-get update && \
    apt-get install -y gcc libc6-dev make

WORKDIR /app

COPY src src
COPY Makefile Makefile

RUN make all

EXPOSE 8000
