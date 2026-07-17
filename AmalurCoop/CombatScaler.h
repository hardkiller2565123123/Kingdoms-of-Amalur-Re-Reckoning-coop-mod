#pragma once

namespace CombatScaler
{
    void Initialize();
    void Shutdown();
    void Update();

    float ScaleBossHealth(float baseHealth);
    float ScaleEnemyHealth(float baseHealth);
    float ScaleBossDamage(float baseDamage);
    float ScaleEnemyDamage(float baseDamage);
}
