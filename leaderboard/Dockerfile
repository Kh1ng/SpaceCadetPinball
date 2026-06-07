FROM oven/bun:1-alpine

WORKDIR /app

COPY package.json bun.lockb* ./
RUN bun install --frozen-lockfile 2>/dev/null || bun install

COPY server.ts ./

RUN mkdir -p /data

VOLUME ["/data"]

ENV DB_PATH=/data/leaderboard.db
ENV PORT=3456

EXPOSE 3456

CMD ["bun", "run", "server.ts"]
