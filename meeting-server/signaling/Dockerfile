ARG GOLANG_IMAGE=golang:1.22-alpine
ARG ALPINE_IMAGE=alpine:3.20

FROM ${GOLANG_IMAGE} AS build

ARG GOPROXY=https://goproxy.cn,direct
ARG GOSUMDB=sum.golang.google.cn

ENV GOPROXY=${GOPROXY}
ENV GOSUMDB=${GOSUMDB}

RUN apk add --no-cache protobuf-dev

WORKDIR /src/signaling

COPY signaling/go.mod signaling/go.sum ./
RUN go mod download \
    && go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.34.2

COPY proto /src/proto
COPY signaling /src/signaling
RUN rm -rf /src/signaling/protocol/pb \
    && mkdir -p /src/signaling/protocol/pb \
    && protoc --proto_path=/src/proto \
        --go_out=/src/signaling/protocol/pb \
        --go_opt=paths=source_relative \
        /src/proto/signaling.proto \
        /src/proto/sfu_rpc.proto

RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/signaling .

FROM ${ALPINE_IMAGE}

RUN apk add --no-cache ca-certificates tzdata \
    && adduser -D -H -u 10001 appuser

WORKDIR /app
COPY --from=build /out/signaling /app/signaling

USER appuser
EXPOSE 8443

ENTRYPOINT ["/app/signaling"]

