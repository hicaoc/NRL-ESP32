FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/nrl-ota .

FROM gcr.io/distroless/static-debian12
WORKDIR /data
COPY --from=build /out/nrl-ota /nrl-ota
EXPOSE 8080
ENTRYPOINT ["/nrl-ota", "-listen", "0.0.0.0:8080", "-data-dir", "/data"]
