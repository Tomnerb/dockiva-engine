FROM scratch
COPY server.elf /app/server
COPY index.html /www/index.html
ENTRYPOINT ["/app/server"]
CMD ["18080", "0", "--bind-any"]
