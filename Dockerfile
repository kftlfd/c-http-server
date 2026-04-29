# ========== Build

FROM alpine:3.22.4 AS build

RUN apk add build-base

WORKDIR /app

COPY src src
COPY Makefile Makefile

RUN make all

# ========== Final

FROM alpine:3.22.4

WORKDIR /app

COPY --from=build /app/server /bin

EXPOSE 8000

CMD [ "server", "--host=0.0.0.0", "--port=8000" ]
