#pragma once

#include <stdint.h>

namespace MarioRun {

constexpr int PLAYER_WIDTH = 9;
constexpr int PLAYER_HEIGHT = 12;
constexpr int GROUND_Y = 55;
constexpr int FLAG_X = 840;

enum class BlockType : uint8_t { Ground, Brick, Question, Pipe };
enum class Status : uint8_t { Playing, Lost, Won };

struct Block {
  int x;
  int y;
  int width;
  int height;
  BlockType type;
};

constexpr Block BLOCKS[] = {
    {0, 55, 380, 9, BlockType::Ground},
    {396, 55, 264, 9, BlockType::Ground},
    {677, 55, 223, 9, BlockType::Ground},
    {88, 31, 8, 8, BlockType::Question},
    {112, 31, 8, 8, BlockType::Brick},
    {120, 31, 8, 8, BlockType::Question},
    {128, 31, 8, 8, BlockType::Brick},
    {180, 43, 12, 12, BlockType::Pipe},
    {260, 39, 12, 16, BlockType::Pipe},
    {310, 31, 8, 8, BlockType::Brick},
    {318, 31, 8, 8, BlockType::Question},
    {326, 31, 8, 8, BlockType::Brick},
    {450, 41, 12, 14, BlockType::Pipe},
    {510, 31, 8, 8, BlockType::Brick},
    {518, 31, 8, 8, BlockType::Question},
    {526, 31, 8, 8, BlockType::Brick},
    {590, 45, 10, 10, BlockType::Brick},
    {600, 35, 10, 20, BlockType::Brick},
    {720, 43, 12, 12, BlockType::Pipe},
    {770, 45, 10, 10, BlockType::Brick},
    {780, 35, 10, 20, BlockType::Brick},
};
constexpr unsigned BLOCK_COUNT = sizeof(BLOCKS) / sizeof(BLOCKS[0]);

struct Enemy {
  float x;
  bool alive;
};

constexpr unsigned ENEMY_COUNT = 5;

struct World {
  float x = 12;
  float y = GROUND_Y - PLAYER_HEIGHT;
  float velocityY = 0;
  bool grounded = true;
  uint8_t jumpFrames = 0;
  uint32_t usedBlocks = 0;
  uint16_t coins = 0;
  uint16_t stomps = 0;
  Enemy enemies[ENEMY_COUNT] = {{195, true}, {275, true}, {345, true},
                               {480, true}, {700, true}};
  Status status = Status::Playing;

  void step(bool pressed, bool held) {
    if (status != Status::Playing) return;
    if (pressed && grounded) {
      velocityY = -3.8f;
      grounded = false;
      jumpFrames = 0;
    }

    float nextX = x + 0.85f;
    for (const Block &block : BLOCKS) {
      if (y < block.y + block.height && y + PLAYER_HEIGHT > block.y &&
          x + PLAYER_WIDTH <= block.x && nextX + PLAYER_WIDTH > block.x) {
        nextX = block.x - PLAYER_WIDTH;
      }
    }
    x = nextX;

    const float previousY = y;
    if (velocityY < 0 && held && jumpFrames < 8) velocityY += 0.17f;
    else velocityY += 0.48f;
    if (velocityY < 0) ++jumpFrames;
    y += velocityY;
    grounded = false;

    for (unsigned index = 0; index < BLOCK_COUNT; ++index) {
      const Block &block = BLOCKS[index];
      if (x + PLAYER_WIDTH <= block.x || x >= block.x + block.width) continue;
      if (velocityY >= 0 && previousY + PLAYER_HEIGHT <= block.y &&
          y + PLAYER_HEIGHT >= block.y) {
        y = block.y - PLAYER_HEIGHT;
        velocityY = 0;
        grounded = true;
      } else if (velocityY < 0 && previousY >= block.y + block.height &&
                 y <= block.y + block.height) {
        y = block.y + block.height;
        velocityY = 0;
        if (block.type == BlockType::Question && !(usedBlocks & (1UL << index))) {
          usedBlocks |= 1UL << index;
          ++coins;
        }
      }
    }

    for (Enemy &enemy : enemies) {
      if (!enemy.alive) continue;
      enemy.x -= 0.12f;
      if (x + PLAYER_WIDTH <= enemy.x || x >= enemy.x + 9 ||
          y + PLAYER_HEIGHT <= GROUND_Y - 8 || y >= GROUND_Y) continue;
      if (velocityY > 0 && previousY + PLAYER_HEIGHT <= GROUND_Y - 6) {
        enemy.alive = false;
        ++stomps;
        y = GROUND_Y - 8 - PLAYER_HEIGHT;
        velocityY = -2.7f;
      } else {
        status = Status::Lost;
        return;
      }
    }
    if (y > 64) status = Status::Lost;
    if (x >= FLAG_X) status = Status::Won;
  }
};

}