#include "../src/world.h"

#include <cassert>
#include <cstdio>
#include <cstdint>

using namespace MarioRun;

float jumpPeak(bool held) {
  World world;
  float peak = world.y;
  for (int frame = 0; frame < 60; ++frame) {
    world.step(frame == 0, held);
    if (world.y < peak) peak = world.y;
  }
  return peak;
}

bool completeCourse() {
  uint32_t randomState = 1;
  float furthest = 0;
  for (int attempt = 0; attempt < 12000; ++attempt) {
    World world;
    int holdFrames = 0;
    for (int frame = 0; frame < 2500 && world.status == Status::Playing; ++frame) {
      bool hazardAhead = false;
      for (const Block &block : BLOCKS) {
        if (block.type != BlockType::Ground && block.y + block.height >= GROUND_Y &&
            block.x >= world.x && block.x - (world.x + PLAYER_WIDTH) < 20) {
          hazardAhead = true;
        }
      }
      for (const Enemy &enemy : world.enemies) {
        if (enemy.alive && enemy.x >= world.x &&
            enemy.x - (world.x + PLAYER_WIDTH) < 20) hazardAhead = true;
      }
      if ((world.x > 354 && world.x < 380) || (world.x > 634 && world.x < 660)) {
        hazardAhead = true;
      }
      randomState = randomState * 1664525UL + 1013904223UL;
      const bool jump = world.grounded && hazardAhead && (randomState >> 28) < 3;
      if (jump) holdFrames = 10;
      world.step(jump, holdFrames > 0);
      if (holdFrames > 0) --holdFrames;
    }
    if (world.status == Status::Won) return true;
    if (world.x > furthest) furthest = world.x;
  }
  std::fprintf(stderr, "Furthest simulated position: %.1f\n", furthest);
  return false;
}

int main() {
  assert(jumpPeak(true) < jumpPeak(false) - 10);

  World landing;
  landing.x = 86;
  landing.y = 10;
  landing.velocityY = 2;
  landing.grounded = false;
  for (int frame = 0; frame < 8; ++frame) landing.step(false, false);
  assert(landing.grounded && landing.y == 31 - PLAYER_HEIGHT);

  World bump;
  bump.x = 84;
  bump.step(true, true);
  bump.step(false, true);
  assert(bump.coins == 1);
  assert(bump.usedBlocks & (1UL << 3));
  bump.y = GROUND_Y - PLAYER_HEIGHT;
  bump.velocityY = 0;
  bump.grounded = true;
  bump.step(true, true);
  bump.step(false, true);
  assert(bump.coins == 1);

  World stomp;
  stomp.x = 140;
  stomp.enemies[0].x = 150;
  stomp.y = 28;
  stomp.velocityY = 3;
  stomp.grounded = false;
  for (int frame = 0; frame < 6 && stomp.enemies[0].alive; ++frame) {
    stomp.step(false, false);
  }
  assert(stomp.status == Status::Playing && stomp.stomps == 1);

  World sideCollision;
  sideCollision.x = 140;
  sideCollision.enemies[0].x = 149;
  sideCollision.step(false, false);
  assert(sideCollision.status == Status::Lost);

  World pit;
  pit.x = 381;
  pit.y = 44;
  pit.grounded = false;
  for (int frame = 0; frame < 30 && pit.status == Status::Playing; ++frame) {
    pit.step(false, false);
  }
  assert(pit.status == Status::Lost);

  World finish;
  finish.x = FLAG_X - 1;
  for (int frame = 0; frame < 4; ++frame) finish.step(false, false);
  assert(finish.status == Status::Won);
  assert(completeCourse());
  std::puts("world physics tests passed");
}