FROM docker.io/library/gcc:14-bookworm AS build
WORKDIR /app
COPY . .
RUN make all

FROM docker.io/library/debian:bookworm-slim
COPY --from=build /app/build/keyjson /usr/local/bin/keyjson
WORKDIR /usr/share/doc/keyjson
COPY --from=build /app/LICENSE ./LICENSE
COPY --from=build /app/third_party/yyjson/LICENSE ./LICENSE-yyjson
WORKDIR /
USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/keyjson", "0.0.0.0", "8080"]
