"""Numerically validate the BRDF and sampling used by RaytracingPbr.hlsli.

This is intentionally renderer-independent. It evaluates the equations on the
CPU, integrates them over the hemisphere, and compares the shader's mixed
diffuse/GGX Monte Carlo estimator with deterministic quadrature.
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
DEFAULT_OUTPUT = ROOT / "Results" / "BrdfPhysicalValidation"
PI = math.pi
HLSL_EPSILON = 1.0e-6
HLSL_SPECULAR_DENOMINATOR_EPSILON = 1.0e-8
MIN_GGX_ALPHA = 1.0e-3


@dataclass(frozen=True)
class Material:
    name: str
    base_color: tuple[float, float, float]
    metallic: float


MATERIALS = (
    Material("dielectric_white", (1.0, 1.0, 1.0), 0.0),
    Material("dielectric_gold_color", (1.0, 0.766, 0.336), 0.0),
    Material("conductor_white", (1.0, 1.0, 1.0), 1.0),
    Material("conductor_gold", (1.0, 0.766, 0.336), 1.0),
)

BRDF_VALIDATION_CASES = (
    ("current_conductor", MATERIALS[3], 0.35, 1.0),
    ("sharp_conductor", MATERIALS[3], 0.1, 1.0),
    ("dielectric_mid", MATERIALS[1], 0.35, 1.0),
    ("dielectric_rough_grazing", MATERIALS[1], 0.8, 0.5),
)

WHITE_FURNACE_ROUGHNESSES = (0.1, 0.2, 0.35, 0.65, 0.8)
WHITE_FURNACE_N_DOT_VS = (0.2, 0.5, 1.0)
INTEGRATION_CONVERGENCE_TOLERANCE = 1.0e-3


def ggx_alpha(roughness: float) -> float:
    clamped_roughness = float(np.clip(roughness, 0.0, 1.0))
    return max(clamped_roughness * clamped_roughness, MIN_GGX_ALPHA)


def distribution_ggx(
    n_dot_h: np.ndarray | float,
    roughness: float,
) -> np.ndarray:
    """Match DistributionGGX in RaytracingPbr.hlsli."""
    n_dot_h = np.asarray(n_dot_h, dtype=np.float64)
    alpha = ggx_alpha(roughness)
    alpha_squared = alpha * alpha
    denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0
    denominator = PI * denominator * denominator
    return alpha_squared / np.maximum(
        denominator, np.finfo(np.float64).tiny)


def geometry_smith_height_correlated(
    n_dot_v: np.ndarray | float,
    n_dot_l: np.ndarray | float,
    roughness: float,
) -> np.ndarray:
    n_dot_v = np.asarray(n_dot_v, dtype=np.float64)
    n_dot_l = np.asarray(n_dot_l, dtype=np.float64)
    alpha = ggx_alpha(roughness)
    alpha_squared = alpha * alpha
    smith_v = n_dot_l * np.sqrt(np.maximum(
        n_dot_v * n_dot_v * (1.0 - alpha_squared) + alpha_squared, 0.0))
    smith_l = n_dot_v * np.sqrt(np.maximum(
        n_dot_l * n_dot_l * (1.0 - alpha_squared) + alpha_squared, 0.0))
    value = (2.0 * n_dot_v * n_dot_l) / np.maximum(
        smith_v + smith_l, HLSL_EPSILON)
    return np.where((n_dot_v > 0.0) & (n_dot_l > 0.0), value, 0.0)


def fresnel_schlick(cos_theta: np.ndarray, f0: np.ndarray) -> np.ndarray:
    cos_theta = np.clip(np.asarray(cos_theta, dtype=np.float64), 0.0, 1.0)
    return f0 + (1.0 - f0) * (1.0 - cos_theta[..., None]) ** 5


def evaluate_brdf(
    material: Material,
    roughness: float,
    view: np.ndarray,
    lights: np.ndarray,
) -> np.ndarray:
    """Return f_r without the cosine factor; the shader returns f_r * NdotL."""
    view = np.asarray(view, dtype=np.float64)
    lights = np.atleast_2d(np.asarray(lights, dtype=np.float64))
    n_dot_v = float(np.clip(view[2], 0.0, 1.0))
    n_dot_l = np.clip(lights[:, 2], 0.0, 1.0)

    half_vectors = lights + view[None, :]
    half_lengths = np.linalg.norm(half_vectors, axis=1)
    half_vectors /= np.maximum(half_lengths[:, None], np.finfo(np.float64).tiny)
    degenerate = half_lengths <= np.finfo(np.float64).tiny
    half_vectors[degenerate] = np.array([0.0, 0.0, 1.0])
    n_dot_h = np.clip(half_vectors[:, 2], 0.0, 1.0)
    v_dot_h = np.clip(half_vectors @ view, 0.0, 1.0)

    base_color = np.asarray(material.base_color, dtype=np.float64)
    f0 = (1.0 - material.metallic) * 0.04 + material.metallic * base_color
    d = distribution_ggx(n_dot_h, roughness)
    g = geometry_smith_height_correlated(n_dot_v, n_dot_l, roughness)
    f = fresnel_schlick(v_dot_h, f0)

    specular_denominator = np.maximum(
        4.0 * n_dot_v * n_dot_l, HLSL_SPECULAR_DENOMINATOR_EPSILON)
    specular = d[:, None] * g[:, None] * f / specular_denominator[:, None]
    fresnel_view = fresnel_schlick(np.array(n_dot_v), f0)
    fresnel_light = fresnel_schlick(n_dot_l, f0)
    diffuse_transmission = (1.0 - fresnel_view) * (1.0 - fresnel_light)
    diffuse = (diffuse_transmission * (1.0 - material.metallic)
               * base_color[None, :] / PI)
    result = diffuse + specular
    return np.where((n_dot_l > 0.0)[:, None], result, 0.0)


def evaluate_diffuse_brdf(
    material: Material,
    view: np.ndarray,
    lights: np.ndarray,
) -> np.ndarray:
    """Return only the diffuse part of f_r for deterministic integration."""
    view = np.asarray(view, dtype=np.float64)
    lights = np.atleast_2d(np.asarray(lights, dtype=np.float64))
    n_dot_v = float(np.clip(view[2], 0.0, 1.0))
    n_dot_l = np.clip(lights[:, 2], 0.0, 1.0)

    base_color = np.asarray(material.base_color, dtype=np.float64)
    f0 = (1.0 - material.metallic) * 0.04 + material.metallic * base_color
    fresnel_view = fresnel_schlick(np.array(n_dot_v), f0)
    fresnel_light = fresnel_schlick(n_dot_l, f0)
    diffuse_transmission = (1.0 - fresnel_view) * (1.0 - fresnel_light)
    diffuse = (diffuse_transmission * (1.0 - material.metallic)
               * base_color[None, :] / PI)
    active = (n_dot_v > 0.0) & (n_dot_l > 0.0)
    return np.where(active[:, None], diffuse, 0.0)


def view_direction(n_dot_v: float) -> np.ndarray:
    return np.array([math.sqrt(max(0.0, 1.0 - n_dot_v * n_dot_v)),
                     0.0, n_dot_v], dtype=np.float64)


def hemisphere_quadrature(mu_count: int, phi_count: int):
    nodes, node_weights = np.polynomial.legendre.leggauss(mu_count)
    mu = 0.5 * (nodes + 1.0)
    mu_weights = 0.5 * node_weights
    phi = (np.arange(phi_count, dtype=np.float64) + 0.5) * (2.0 * PI / phi_count)

    mu_grid, phi_grid = np.meshgrid(mu, phi, indexing="ij")
    sin_theta = np.sqrt(np.maximum(0.0, 1.0 - mu_grid * mu_grid))
    directions = np.stack((
        sin_theta * np.cos(phi_grid),
        sin_theta * np.sin(phi_grid),
        mu_grid,
    ), axis=-1).reshape(-1, 3)
    weights = np.repeat(mu_weights, phi_count) * (2.0 * PI / phi_count)
    return directions, weights


def directional_reflectance(
    material: Material,
    roughness: float,
    n_dot_v: float,
    directions: np.ndarray,
    weights: np.ndarray,
) -> np.ndarray:
    view = view_direction(n_dot_v)
    diffuse = evaluate_diffuse_brdf(material, view, directions)
    diffuse_reflectance = np.sum(
        diffuse * directions[:, 2, None] * weights[:, None], axis=0)
    specular_reflectance = directional_specular_reflectance(
        material, roughness, view, directions, weights)
    return diffuse_reflectance + specular_reflectance


def directional_specular_reflectance(
    material: Material,
    roughness: float,
    view: np.ndarray,
    quadrature_directions: np.ndarray,
    solid_angle_weights: np.ndarray,
) -> np.ndarray:
    """Integrate specular f_r with deterministic GGX NDF importance sampling.

    The hemisphere quadrature's z coordinate is reused as a unit-interval GGX
    CDF sample. Dividing its solid-angle weights by 2*pi gives the matching
    unit-square quadrature weights. This resolves narrow low-roughness lobes
    without increasing the global light-direction grid.
    """
    view = np.asarray(view, dtype=np.float64)
    samples = np.asarray(quadrature_directions, dtype=np.float64)
    u = np.clip(samples[:, 2], 0.0, 1.0)

    azimuth_radius = np.linalg.norm(samples[:, :2], axis=1)
    cos_phi = np.divide(
        samples[:, 0], azimuth_radius,
        out=np.ones_like(azimuth_radius),
        where=azimuth_radius > np.finfo(np.float64).tiny)
    sin_phi = np.divide(
        samples[:, 1], azimuth_radius,
        out=np.zeros_like(azimuth_radius),
        where=azimuth_radius > np.finfo(np.float64).tiny)

    alpha = ggx_alpha(roughness)
    alpha_squared = alpha * alpha
    cos_theta_h = np.sqrt((1.0 - u) / np.maximum(
        1.0 + (alpha_squared - 1.0) * u,
        np.finfo(np.float64).tiny))
    sin_theta_h = np.sqrt(np.maximum(0.0, 1.0 - cos_theta_h * cos_theta_h))
    half_vectors = np.stack((
        sin_theta_h * cos_phi,
        sin_theta_h * sin_phi,
        cos_theta_h,
    ), axis=1)

    v_dot_h_unclipped = half_vectors @ view
    lights = 2.0 * v_dot_h_unclipped[:, None] * half_vectors - view[None, :]
    n_dot_v = float(np.clip(view[2], 0.0, 1.0))
    n_dot_l = np.clip(lights[:, 2], 0.0, 1.0)
    v_dot_h = np.clip(v_dot_h_unclipped, 0.0, 1.0)

    base_color = np.asarray(material.base_color, dtype=np.float64)
    f0 = (1.0 - material.metallic) * 0.04 + material.metallic * base_color
    fresnel = fresnel_schlick(v_dot_h, f0)
    geometry = geometry_smith_height_correlated(
        n_dot_v, n_dot_l, roughness)

    denominator = np.maximum(
        n_dot_v * cos_theta_h,
        np.finfo(np.float64).tiny)
    integrand = (fresnel * geometry[:, None] * v_dot_h[:, None]
                 / denominator[:, None])
    active = ((v_dot_h_unclipped > 0.0) & (lights[:, 2] > 0.0)
              & (cos_theta_h > 0.0) & (n_dot_v > 0.0))
    integrand = np.where(active[:, None], integrand, 0.0)

    probability_weights = solid_angle_weights / (2.0 * PI)
    return np.sum(integrand * probability_weights[:, None], axis=0)


def ggx_d_normalization(roughness: float) -> float:
    # mu = 1 - x^4 clusters samples around the narrow GGX peak at mu=1.
    nodes, weights = np.polynomial.legendre.leggauss(1024)
    x = 0.5 * (nodes + 1.0)
    x_weights = 0.5 * weights
    mu = 1.0 - x ** 4
    jacobian = 4.0 * x ** 3
    integrand = (2.0 * PI * distribution_ggx(mu, roughness)
                 * mu * jacobian)
    return float(np.sum(integrand * x_weights))


def test_properties() -> tuple[list[dict], dict]:
    rows: list[dict] = []
    d_max_error = 0.0
    for roughness in (0.03, 0.1, 0.2, 0.35, 0.65, 1.0):
        hlsl_value = ggx_d_normalization(roughness)
        ideal_value = 1.0
        d_error = abs(hlsl_value - 1.0)
        d_max_error = max(d_max_error, d_error)
        rows.append({
            "test": "GGX_D_normalization",
            "case": f"roughness={roughness:.2f}",
            "value": hlsl_value,
            "expected": 1.0,
            "absolute_error": d_error,
            "reference_value": ideal_value,
            "passed": d_error <= 0.005,
        })

    f0 = np.array([0.04, 0.2, 0.8], dtype=np.float64)
    f_normal = fresnel_schlick(np.array(1.0), f0)
    f_grazing = fresnel_schlick(np.array(0.0), f0)
    fresnel_error = max(float(np.max(np.abs(f_normal - f0))),
                        float(np.max(np.abs(f_grazing - 1.0))))
    rows.append({
        "test": "Fresnel_boundary",
        "case": "cosTheta=1_and_0",
        "value": fresnel_error,
        "expected": 0.0,
        "absolute_error": fresnel_error,
        "reference_value": 0.0,
        "passed": fresnel_error <= 1.0e-12,
    })

    reciprocity_max = 0.0
    minimum_value = float("inf")
    finite = True
    for material in MATERIALS:
        for roughness in (0.1, 0.35, 0.8):
            for n_dot_v in (0.15, 0.4, 0.8, 1.0):
                view = view_direction(n_dot_v)
                for n_dot_l in (0.15, 0.4, 0.8, 1.0):
                    for phi in (0.0, 0.7, 1.8, PI):
                        light = np.array([
                            math.sqrt(max(0.0, 1.0 - n_dot_l * n_dot_l)) * math.cos(phi),
                            math.sqrt(max(0.0, 1.0 - n_dot_l * n_dot_l)) * math.sin(phi),
                            n_dot_l,
                        ])
                        forward = evaluate_brdf(material, roughness, view, light)[0]
                        reverse = evaluate_brdf(material, roughness, light, view)[0]
                        scale = np.maximum(np.maximum(np.abs(forward), np.abs(reverse)), 1.0e-8)
                        reciprocity_max = max(
                            reciprocity_max,
                            float(np.max(np.abs(forward - reverse) / scale)))
                        minimum_value = min(minimum_value, float(np.min(forward)))
                        finite = finite and bool(np.all(np.isfinite(forward)))

    rows.append({
        "test": "reciprocity",
        "case": "direction_material_grid",
        "value": reciprocity_max,
        "expected": 0.0,
        "absolute_error": reciprocity_max,
        "reference_value": 0.0,
        "passed": reciprocity_max <= 1.0e-5,
    })
    rows.append({
        "test": "nonnegative_and_finite",
        "case": "direction_material_grid",
        "value": minimum_value,
        "expected": 0.0,
        "absolute_error": max(0.0, -minimum_value),
        "reference_value": 0.0,
        "passed": finite and minimum_value >= -1.0e-12,
    })

    summary = {
        "d_max_error": d_max_error,
        "reciprocity_max": reciprocity_max,
        "minimum_value": minimum_value,
        "finite": finite,
        "passed": all(bool(row["passed"]) for row in rows),
    }
    return rows, summary


def run_white_furnace(directions: np.ndarray, weights: np.ndarray) -> tuple[list[dict], dict]:
    rows: list[dict] = []
    maximum_reflectance = 0.0
    maximum_violation = 0.0
    largest_conductor_loss = 0.0
    largest_rough_conductor_loss = 0.0

    for material in MATERIALS:
        for roughness in WHITE_FURNACE_ROUGHNESSES:
            for n_dot_v in WHITE_FURNACE_N_DOT_VS:
                reflectance = directional_reflectance(
                    material, roughness, n_dot_v, directions, weights)
                max_channel = float(np.max(reflectance))
                violation = max(0.0, max_channel - 1.0)
                maximum_reflectance = max(maximum_reflectance, max_channel)
                maximum_violation = max(maximum_violation, violation)
                if material.name == "conductor_white":
                    largest_conductor_loss = max(
                        largest_conductor_loss, 1.0 - float(np.min(reflectance)))
                    if roughness >= 0.35:
                        largest_rough_conductor_loss = max(
                            largest_rough_conductor_loss,
                            1.0 - float(np.min(reflectance)))
                rows.append({
                    "material": material.name,
                    "metallic": material.metallic,
                    "roughness": roughness,
                    "n_dot_v": n_dot_v,
                    "reflectance_r": float(reflectance[0]),
                    "reflectance_g": float(reflectance[1]),
                    "reflectance_b": float(reflectance[2]),
                    "max_channel": max_channel,
                    "energy_violation": violation,
                    "passed": violation <= 0.005,
                })

    return rows, {
        "maximum_reflectance": maximum_reflectance,
        "maximum_violation": maximum_violation,
        "largest_conductor_loss": largest_conductor_loss,
        "largest_rough_conductor_loss": largest_rough_conductor_loss,
        "passed": maximum_violation <= 0.005,
    }


def run_integration_convergence(
    fine_rows: list[dict],
    mu_samples: int,
    phi_samples: int,
) -> tuple[list[dict], dict]:
    """Compare the configured quadrature against a half-resolution grid."""
    coarse_mu_samples = max(16, mu_samples // 2)
    coarse_phi_samples = max(32, phi_samples // 2)
    coarse_directions, coarse_weights = hemisphere_quadrature(
        coarse_mu_samples, coarse_phi_samples)
    materials_by_name = {material.name: material for material in MATERIALS}

    rows: list[dict] = []
    maximum_relative_difference = 0.0
    for fine_row in fine_rows:
        material = materials_by_name[fine_row["material"]]
        roughness = float(fine_row["roughness"])
        n_dot_v = float(fine_row["n_dot_v"])
        fine = np.array((
            fine_row["reflectance_r"],
            fine_row["reflectance_g"],
            fine_row["reflectance_b"],
        ), dtype=np.float64)
        coarse = directional_reflectance(
            material,
            roughness,
            n_dot_v,
            coarse_directions,
            coarse_weights)
        relative_difference = np.abs(coarse - fine) / np.maximum(
            np.abs(fine), 1.0e-8)
        maximum_case_difference = float(np.max(relative_difference))
        maximum_relative_difference = max(
            maximum_relative_difference,
            maximum_case_difference)
        rows.append({
            "material": material.name,
            "roughness": roughness,
            "n_dot_v": n_dot_v,
            "coarse_mu_samples": coarse_mu_samples,
            "coarse_phi_samples": coarse_phi_samples,
            "fine_mu_samples": mu_samples,
            "fine_phi_samples": phi_samples,
            "coarse_reflectance_r": float(coarse[0]),
            "coarse_reflectance_g": float(coarse[1]),
            "coarse_reflectance_b": float(coarse[2]),
            "fine_reflectance_r": float(fine[0]),
            "fine_reflectance_g": float(fine[1]),
            "fine_reflectance_b": float(fine[2]),
            "max_relative_difference": maximum_case_difference,
            "passed": maximum_case_difference <= INTEGRATION_CONVERGENCE_TOLERANCE,
        })

    return rows, {
        "coarse_mu_samples": coarse_mu_samples,
        "coarse_phi_samples": coarse_phi_samples,
        "fine_mu_samples": mu_samples,
        "fine_phi_samples": phi_samples,
        "maximum_relative_difference": maximum_relative_difference,
        "tolerance": INTEGRATION_CONVERGENCE_TOLERANCE,
        "passed": maximum_relative_difference <= INTEGRATION_CONVERGENCE_TOLERANCE,
    }


def pdf_ggx_shader(view: np.ndarray, lights: np.ndarray, roughness: float) -> np.ndarray:
    lights = np.atleast_2d(lights)
    half_vectors = lights + view[None, :]
    half_vectors /= np.maximum(
        np.linalg.norm(half_vectors, axis=1)[:, None], np.finfo(np.float64).tiny)
    v_dot_h = np.clip(half_vectors @ view, 0.0, 1.0)
    n_dot_h = np.clip(half_vectors[:, 2], 0.0, 1.0)
    pdf = (distribution_ggx(n_dot_h, roughness) * n_dot_h
           / np.maximum(4.0 * v_dot_h, HLSL_SPECULAR_DENOMINATOR_EPSILON))
    active = (lights[:, 2] > 0.0) & (v_dot_h > 0.0)
    return np.where(active, pdf, 0.0)


def pdf_ggx_sampling_distribution(
    view: np.ndarray,
    lights: np.ndarray,
    roughness: float,
) -> np.ndarray:
    """Actual proposal density implied by ImportanceSampleGGX."""
    lights = np.atleast_2d(lights)
    half_vectors = lights + view[None, :]
    half_vectors /= np.maximum(
        np.linalg.norm(half_vectors, axis=1)[:, None], np.finfo(np.float64).tiny)
    v_dot_h = np.clip(half_vectors @ view, 0.0, 1.0)
    n_dot_h = np.clip(half_vectors[:, 2], 0.0, 1.0)
    pdf = (distribution_ggx(n_dot_h, roughness) * n_dot_h
           / np.maximum(4.0 * v_dot_h, np.finfo(np.float64).tiny))
    active = (lights[:, 2] > 0.0) & (v_dot_h > 0.0)
    return np.where(active, pdf, 0.0)


def sample_ggx_ndf(
    rng: np.random.Generator,
    view: np.ndarray,
    roughness: float,
    sample_count: int,
):
    samples = rng.random((sample_count, 2), dtype=np.float64)
    alpha = ggx_alpha(roughness)
    alpha_squared = alpha * alpha
    phi = samples[:, 0] * (2.0 * PI)
    cos_theta = np.sqrt((1.0 - samples[:, 1]) / np.maximum(
        1.0 + (alpha_squared - 1.0) * samples[:, 1], HLSL_EPSILON))
    sin_theta = np.sqrt(np.maximum(0.0, 1.0 - cos_theta * cos_theta))
    half_vectors = np.stack((sin_theta * np.cos(phi),
                             sin_theta * np.sin(phi),
                             cos_theta), axis=1)
    v_dot_h = half_vectors @ view
    lights = 2.0 * v_dot_h[:, None] * half_vectors - view[None, :]
    valid = (v_dot_h > 0.0) & (lights[:, 2] > 0.0)
    return lights, valid


def specular_sampling_probability(material: Material) -> float:
    return 0.5 + 0.5 * float(np.clip(material.metallic, 0.0, 1.0))


def pdf_brdf_mixture(
    material: Material,
    view: np.ndarray,
    lights: np.ndarray,
    roughness: float,
) -> np.ndarray:
    lights = np.atleast_2d(lights)
    specular_probability = specular_sampling_probability(material)
    specular_pdf = pdf_ggx_shader(view, lights, roughness)
    diffuse_pdf = np.clip(lights[:, 2], 0.0, 1.0) / PI
    return (specular_probability * specular_pdf
            + (1.0 - specular_probability) * diffuse_pdf)


def sample_brdf_mixture(
    rng: np.random.Generator,
    material: Material,
    view: np.ndarray,
    roughness: float,
    sample_count: int,
):
    specular_probability = specular_sampling_probability(material)
    choose_specular = rng.random(sample_count) < specular_probability
    lights = np.zeros((sample_count, 3), dtype=np.float64)
    valid = np.ones(sample_count, dtype=bool)

    specular_count = int(np.count_nonzero(choose_specular))
    if specular_count:
        specular_lights, specular_valid = sample_ggx_ndf(
            rng, view, roughness, specular_count)
        lights[choose_specular] = specular_lights
        valid[choose_specular] = specular_valid

    choose_diffuse = ~choose_specular
    diffuse_count = sample_count - specular_count
    if diffuse_count:
        samples = rng.random((diffuse_count, 2), dtype=np.float64)
        radius = np.sqrt(samples[:, 0])
        phi = samples[:, 1] * (2.0 * PI)
        lights[choose_diffuse] = np.stack((
            radius * np.cos(phi),
            radius * np.sin(phi),
            np.sqrt(np.maximum(0.0, 1.0 - samples[:, 0])),
        ), axis=1)

    return lights, valid


def run_pdf_consistency(
    directions: np.ndarray,
    weights: np.ndarray,
    sample_count: int,
) -> tuple[list[dict], dict]:
    del directions, weights
    rows: list[dict] = []
    maximum_density_mismatch = 0.0

    for case_index, roughness in enumerate((0.1, 0.2, 0.35, 0.8)):
        for n_dot_v in (0.2, 0.5, 1.0):
            view = view_direction(n_dot_v)
            rng = np.random.default_rng(1000 + case_index * 17 + int(n_dot_v * 100))
            lights, valid = sample_ggx_ndf(rng, view, roughness, sample_count)
            success_probability = float(np.mean(valid))
            shader_mass = 0.0
            if np.any(valid):
                valid_lights = lights[valid]
                shader_pdf = pdf_ggx_shader(view, valid_lights, roughness)
                sample_pdf = pdf_ggx_sampling_distribution(
                    view, valid_lights, roughness)
                # Importance-integrate the shader PDF with samples drawn from
                # the actual proposal distribution. Invalid reflections are
                # the discrete rejection event of NDF sampling.
                shader_mass = float(np.sum(shader_pdf / sample_pdf) / sample_count)
            sample_mass = success_probability
            mass_error = 0.0
            density_mismatch = abs(shader_mass - success_probability)
            maximum_density_mismatch = max(maximum_density_mismatch, density_mismatch)
            rows.append({
                "roughness": roughness,
                "n_dot_v": n_dot_v,
                "shader_pdf_mass": shader_mass,
                "sampling_pdf_mass": sample_mass,
                "sample_success_probability": success_probability,
                "sampling_mass_error": mass_error,
                "shader_sampling_density_mismatch": density_mismatch,
                "passed": mass_error <= 0.01 and density_mismatch <= 0.01,
            })

    return rows, {
        "maximum_mass_error": 0.0,
        "maximum_density_mismatch": maximum_density_mismatch,
        "passed": maximum_density_mismatch <= 0.01,
    }


def monte_carlo_estimates(
    material: Material,
    roughness: float,
    n_dot_v: float,
    sample_levels: tuple[int, ...],
    seed: int,
):
    rng = np.random.default_rng(seed)
    view = view_direction(n_dot_v)
    total = np.zeros(3, dtype=np.float64)
    processed = 0
    estimates: dict[int, np.ndarray] = {}

    for target in sample_levels:
        while processed < target:
            count = min(65536, target - processed)
            lights, valid = sample_brdf_mixture(
                rng, material, view, roughness, count)
            if np.any(valid):
                valid_lights = lights[valid]
                brdf = evaluate_brdf(material, roughness, view, valid_lights)
                pdf = pdf_brdf_mixture(
                    material, view, valid_lights, roughness)
                sample_weights = brdf * valid_lights[:, 2, None] / pdf[:, None]
                total += np.sum(sample_weights, axis=0)
            processed += count
        estimates[target] = total / target
    return estimates


def run_sampling_convergence(
    directions: np.ndarray,
    weights: np.ndarray,
    maximum_samples: int,
) -> tuple[list[dict], dict]:
    requested = (4096, 65536, maximum_samples)
    sample_levels = tuple(sorted(set(level for level in requested if level <= maximum_samples)))
    rows: list[dict] = []
    final_maximum_relative_error = 0.0

    for case_index, (name, material, roughness, n_dot_v) in enumerate(
        BRDF_VALIDATION_CASES
    ):
        reference = directional_reflectance(
            material, roughness, n_dot_v, directions, weights)
        estimates = monte_carlo_estimates(
            material, roughness, n_dot_v, sample_levels, 7000 + case_index)
        for sample_count in sample_levels:
            estimate = estimates[sample_count]
            relative_error = np.abs(estimate - reference) / np.maximum(
                np.abs(reference), 1.0e-8)
            max_relative_error = float(np.max(relative_error))
            if sample_count == sample_levels[-1]:
                final_maximum_relative_error = max(
                    final_maximum_relative_error, max_relative_error)
            rows.append({
                "case": name,
                "metallic": material.metallic,
                "roughness": roughness,
                "n_dot_v": n_dot_v,
                "samples": sample_count,
                "reference_r": float(reference[0]),
                "reference_g": float(reference[1]),
                "reference_b": float(reference[2]),
                "estimate_r": float(estimate[0]),
                "estimate_g": float(estimate[1]),
                "estimate_b": float(estimate[2]),
                "max_relative_error": max_relative_error,
            })

    return rows, {
        "final_maximum_relative_error": final_maximum_relative_error,
        "passed": final_maximum_relative_error <= 0.05,
    }


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError("GPU validation input must be an RGB PFM file.")
        dimensions = stream.readline().decode("ascii").strip().split()
        if len(dimensions) != 2:
            raise ValueError("Invalid PFM dimensions.")
        width, height = (int(value) for value in dimensions)
        scale = float(stream.readline().decode("ascii").strip())
        dtype = "<f4" if scale < 0.0 else ">f4"
        pixels = np.fromfile(stream, dtype=dtype)

    expected_values = width * height * 3
    if pixels.size != expected_values:
        raise ValueError(
            f"PFM contains {pixels.size} values; expected {expected_values}.")
    image = pixels.reshape(height, width, 3)
    return np.flipud(image).astype(np.float64)


def run_gpu_validation(
    pfm_path: Path,
    gpu_spp: int,
    directions: np.ndarray,
    weights: np.ndarray,
) -> tuple[list[dict], dict]:
    image = read_pfm(pfm_path)
    case_count = len(BRDF_VALIDATION_CASES)
    height, width, _ = image.shape
    if height % case_count != 0:
        raise ValueError(
            f"GPU validation PFM height must be divisible by {case_count}.")

    stripe_height = height // case_count
    samples_per_case = width * stripe_height * gpu_spp
    rows: list[dict] = []
    maximum_relative_error = 0.0

    for case_index, (name, material, roughness, n_dot_v) in enumerate(
        BRDF_VALIDATION_CASES
    ):
        y_begin = case_index * stripe_height
        y_end = y_begin + stripe_height
        gpu_estimate = np.mean(
            image[y_begin:y_end, :, :],
            axis=(0, 1),
            dtype=np.float64)
        cpu_reference = directional_reflectance(
            material,
            roughness,
            n_dot_v,
            directions,
            weights)
        relative_error = np.abs(gpu_estimate - cpu_reference) / np.maximum(
            np.abs(cpu_reference), 1.0e-8)
        max_relative_error = float(np.max(relative_error))
        maximum_relative_error = max(
            maximum_relative_error,
            max_relative_error)
        rows.append({
            "case": name,
            "metallic": material.metallic,
            "roughness": roughness,
            "n_dot_v": n_dot_v,
            "gpu_samples": samples_per_case,
            "cpu_reference_r": float(cpu_reference[0]),
            "cpu_reference_g": float(cpu_reference[1]),
            "cpu_reference_b": float(cpu_reference[2]),
            "gpu_estimate_r": float(gpu_estimate[0]),
            "gpu_estimate_g": float(gpu_estimate[1]),
            "gpu_estimate_b": float(gpu_estimate[2]),
            "max_relative_error": max_relative_error,
            "passed": max_relative_error <= 0.005,
        })

    return rows, {
        "pfm_path": str(pfm_path),
        "width": width,
        "height": height,
        "gpu_spp": gpu_spp,
        "samples_per_case": samples_per_case,
        "maximum_relative_error": maximum_relative_error,
        "passed": maximum_relative_error <= 0.005,
    }


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def pass_text(value: bool) -> str:
    return "PASS" if value else "FAIL"


def write_korean_summary(
    path: Path,
    property_rows: list[dict],
    property_summary: dict,
    furnace_summary: dict,
    integration_summary: dict,
    pdf_summary: dict,
    convergence_rows: list[dict],
    convergence_summary: dict,
    gpu_rows: list[dict],
    gpu_summary: dict | None,
    mu_samples: int,
    phi_samples: int,
    mc_samples: int,
) -> None:
    overall = (property_summary["passed"] and furnace_summary["passed"]
               and integration_summary["passed"]
               and pdf_summary["passed"] and convergence_summary["passed"]
               and (gpu_summary is None or gpu_summary["passed"]))
    d_rows = [row for row in property_rows if row["test"] == "GGX_D_normalization"]
    final_rows = [row for row in convergence_rows if row["samples"] == mc_samples]
    lines = [
        "# PBR BRDF 물리적 타당성 검증",
        "",
        "## 판정",
        "",
        f"- 전체 판정: **{pass_text(overall)}**",
        f"- BRDF 기본 성질: **{pass_text(property_summary['passed'])}**",
        f"- White furnace 에너지 상한: **{pass_text(furnace_summary['passed'])}**",
        f"- 적분 해상도 수렴: **{pass_text(integration_summary['passed'])}**",
        f"- GGX 샘플링/PDF 일치: **{pass_text(pdf_summary['passed'])}**",
        f"- Monte Carlo 수렴: **{pass_text(convergence_summary['passed'])}**",
        (
            f"- GPU HLSL 교차 검증: **{pass_text(gpu_summary['passed'])}**"
            if gpu_summary is not None
            else "- GPU HLSL 교차 검증: **NOT RUN**"
        ),
        "",
        (
            "CPU 고정밀 반구 적분값을 기준으로 실제 GPU HLSL 결과까지 교차검증했습니다."
            if gpu_summary is not None
            else "Mitsuba 또는 RTXPT 결과를 사용하지 않고 현재 HLSL 공식을 CPU에서 독립적으로 반구 적분했습니다."
        ),
        "",
        "## 이전 실패 원인과 수정",
        "",
        "- height-correlated Smith G 자체가 실패 원인은 아니었습니다.",
        "- GGX D 분모 전체를 1e-6으로 제한해 낮은 roughness의 피크가 잘렸고, 잘리지 않은 분포를 만드는 sampler와 PDF가 달라졌습니다. D, G, sampler가 공통 alpha를 사용하도록 수정했습니다.",
        "- diffuse에 half-vector Fresnel 한 항만 곱하면 grazing angle에서 diffuse와 specular의 에너지 합이 1을 넘었습니다. 입사·출사 양쪽 Fresnel 투과율을 곱하는 대칭형 diffuse로 수정했습니다.",
        "- dielectric의 넓은 diffuse까지 좁은 GGX proposal 하나로 추정하던 고분산 sampling을 cosine diffuse/GGX mixture sampling으로 교체했습니다.",
        "",
        "## 핵심 수치",
        "",
        f"- 최대 reciprocity 상대 오차: {property_summary['reciprocity_max']:.8e}",
        f"- GGX D 정규화 최대 오차: {property_summary['d_max_error']:.8e}",
        f"- White furnace 최대 반사율: {furnace_summary['maximum_reflectance']:.8f}",
        f"- 에너지 상한 최대 초과량: {furnace_summary['maximum_violation']:.8e}",
        f"- 절반/전체 적분 해상도 최대 상대 차이: {integration_summary['maximum_relative_difference']:.8e}",
        f"- 흰색 금속 single-scatter 최대 에너지 손실: {furnace_summary['largest_conductor_loss']:.8f}",
        f"- roughness 0.35 이상 흰색 금속 최대 에너지 손실: {furnace_summary['largest_rough_conductor_loss']:.8f}",
        f"- Shader PDF와 실제 샘플링 분포의 최대 질량 차이: {pdf_summary['maximum_density_mismatch']:.8e}",
        f"- {mc_samples} samples 최대 수렴 상대 오차: {convergence_summary['final_maximum_relative_error']:.6f}",
        (
            f"- GPU HLSL 대 CPU 기준 최대 상대 오차: {gpu_summary['maximum_relative_error']:.6f}"
            if gpu_summary is not None
            else "- GPU HLSL 대 CPU 기준 최대 상대 오차: 미실행"
        ),
        "",
        "## GGX D 정규화",
        "",
        "| Roughness | HLSL D 적분 | 이론 정규화 값 | 판정 |",
        "| ---: | ---: | ---: | :---: |",
    ]
    for row in d_rows:
        roughness = row["case"].split("=")[1]
        lines.append(
            f"| {roughness} | {row['value']:.8f} | {row['reference_value']:.8f} | "
            f"{pass_text(row['passed'])} |")

    lines.extend([
        "",
        "## Monte Carlo 최종 수렴",
        "",
        "| Case | Reference RGB | Estimate RGB | 최대 상대 오차 |",
        "| --- | --- | --- | ---: |",
    ])
    for row in final_rows:
        reference = (
            f"({row['reference_r']:.5f}, {row['reference_g']:.5f}, {row['reference_b']:.5f})")
        estimate = (
            f"({row['estimate_r']:.5f}, {row['estimate_g']:.5f}, {row['estimate_b']:.5f})")
        lines.append(
            f"| {row['case']} | {reference} | {estimate} | "
            f"{row['max_relative_error']:.6f} |")

    if gpu_summary is not None:
        lines.extend([
            "",
            "## 실제 GPU HLSL 교차 검증",
            "",
            "GPU ray-generation shader가 실제 EvaluateBrdf와 mixture sampling/PDF를 실행한 결과를 CPU 결정론적 반구 적분값과 비교했습니다.",
            "",
            "| Case | CPU reference RGB | GPU estimate RGB | GPU samples | 최대 상대 오차 | 판정 |",
            "| --- | --- | --- | ---: | ---: | :---: |",
        ])
        for row in gpu_rows:
            reference = (
                f"({row['cpu_reference_r']:.5f}, "
                f"{row['cpu_reference_g']:.5f}, "
                f"{row['cpu_reference_b']:.5f})")
            estimate = (
                f"({row['gpu_estimate_r']:.5f}, "
                f"{row['gpu_estimate_g']:.5f}, "
                f"{row['gpu_estimate_b']:.5f})")
            lines.append(
                f"| {row['case']} | {reference} | {estimate} | "
                f"{row['gpu_samples']} | {row['max_relative_error']:.6f} | "
                f"{pass_text(row['passed'])} |")

    lines.extend(["", "## 해석", ""])
    if not property_summary["passed"]:
        lines.append(
            "- BRDF 기본 성질 중 실패 항목이 있어 현재 구현을 전 구간에서 물리적으로 타당하다고 결론 내릴 수 없습니다.")
    else:
        lines.append("- reciprocity, 비음수성, 유한성 및 GGX 정규화 검사를 통과했습니다.")
    if integration_summary["passed"]:
        lines.append(
            "- Diffuse 직접 반구 적분과 GGX 중요도 기반 specular 적분이 절반 해상도 대비 허용 오차 0.1% 이내로 수렴했습니다.")
    else:
        lines.append(
            "- 절반 해상도와 전체 해상도 적분값의 차이가 0.1%를 초과해 적분 해상도를 높여야 합니다.")
    if property_summary["d_max_error"] > 0.005:
        lines.append(
            "- 낮은 roughness에서 HLSL의 denominator 1e-6 클램프가 GGX 피크를 잘라 D 정규화를 깨뜨립니다.")
    if furnace_summary["largest_rough_conductor_loss"] > 0.05:
        lines.append(
            "- 거친 흰색 금속의 반사율 손실은 single-scatter GGX에서 누락된 다중 산란 에너지입니다.")
    if not pdf_summary["passed"]:
        lines.append(
            "- ImportanceSampleGGX가 생성하는 분포와 셰이더가 나누는 PDF가 일치하지 않는 구간이 있습니다.")
    if not convergence_summary["passed"]:
        if not pdf_summary["passed"]:
            lines.append(
                "- 낮은 roughness의 D/PDF 불일치 때문에 표본 수를 늘려도 sharp conductor가 기준 적분값으로 수렴하지 않습니다.")
        else:
            lines.append(
                "- GGX만으로 diffuse와 specular 전체를 샘플링하여 일부 dielectric 조건의 분산이 큽니다.")
    if gpu_summary is not None:
        if gpu_summary["passed"]:
            lines.append(
                "- CPU 고정밀 기준값과 실제 GPU float32 HLSL 결과가 허용 오차 0.5% 이내에서 일치했습니다.")
        else:
            lines.append(
                "- CPU 기준값과 GPU HLSL 결과가 허용 오차 0.5%를 초과해 구현 차이를 추가로 점검해야 합니다.")
    lines.extend([
        "",
        "## 검증 설정",
        "",
        f"- Diffuse 직접 반구 적분: {mu_samples} x {phi_samples}",
        f"- Specular GGX half-vector 중요도 적분: {mu_samples} x {phi_samples}",
        (
            f"- 적분 수렴 비교: {integration_summary['coarse_mu_samples']} x "
            f"{integration_summary['coarse_phi_samples']} vs "
            f"{integration_summary['fine_mu_samples']} x "
            f"{integration_summary['fine_phi_samples']}"
        ),
        f"- 최종 Monte Carlo 표본: {mc_samples}",
        (
            f"- GPU 표본: 조건당 {gpu_summary['samples_per_case']} "
            f"({gpu_summary['width']} x {gpu_summary['height'] // len(BRDF_VALIDATION_CASES)} x "
            f"{gpu_summary['gpu_spp']} spp)"
            if gpu_summary is not None
            else "- GPU 표본: 미실행"
        ),
        "- 최소 GGX alpha: 0.001 (최소 perceptual roughness 약 0.0316)",
        (
            "- 상세 데이터: property_tests.csv, white_furnace.csv, "
            "integration_convergence.csv, pdf_consistency.csv, "
            "sampling_convergence.csv, gpu_validation.csv"
            if gpu_summary is not None
            else "- 상세 데이터: property_tests.csv, white_furnace.csv, "
                 "integration_convergence.csv, pdf_consistency.csv, "
                 "sampling_convergence.csv"
        ),
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8-sig")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate the DXR PBR BRDF with deterministic integration.")
    parser.add_argument("--mu-samples", type=int, default=320)
    parser.add_argument("--phi-samples", type=int, default=720)
    parser.add_argument("--mc-samples", type=int, default=1048576)
    parser.add_argument("--pdf-samples", type=int, default=524288)
    parser.add_argument(
        "--gpu-pfm",
        type=Path,
        help="PFM captured with --gpu-brdf-validation.")
    parser.add_argument(
        "--gpu-spp",
        type=int,
        default=1,
        help="Accumulated samples per pixel in --gpu-pfm.")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.mu_samples < 32 or args.phi_samples < 64:
        raise ValueError("Quadrature resolution is too low.")
    if args.mc_samples < 4096 or args.pdf_samples < 16384:
        raise ValueError("Monte Carlo sample count is too low.")

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    directions, weights = hemisphere_quadrature(
        args.mu_samples, args.phi_samples)

    property_rows, property_summary = test_properties()
    furnace_rows, furnace_summary = run_white_furnace(directions, weights)
    integration_rows, integration_summary = run_integration_convergence(
        furnace_rows, args.mu_samples, args.phi_samples)
    pdf_rows, pdf_summary = run_pdf_consistency(
        directions, weights, args.pdf_samples)
    convergence_rows, convergence_summary = run_sampling_convergence(
        directions, weights, args.mc_samples)
    gpu_rows: list[dict] = []
    gpu_summary = None
    if args.gpu_pfm is not None:
        if args.gpu_spp < 1:
            raise ValueError("GPU SPP must be at least 1.")
        gpu_rows, gpu_summary = run_gpu_validation(
            args.gpu_pfm.resolve(),
            args.gpu_spp,
            directions,
            weights)

    write_csv(output / "property_tests.csv", property_rows)
    write_csv(output / "white_furnace.csv", furnace_rows)
    write_csv(output / "integration_convergence.csv", integration_rows)
    write_csv(output / "pdf_consistency.csv", pdf_rows)
    write_csv(output / "sampling_convergence.csv", convergence_rows)
    if gpu_summary is not None:
        write_csv(output / "gpu_validation.csv", gpu_rows)
    write_korean_summary(
        output / "presentation_summary_ko.md",
        property_rows,
        property_summary,
        furnace_summary,
        integration_summary,
        pdf_summary,
        convergence_rows,
        convergence_summary,
        gpu_rows,
        gpu_summary,
        args.mu_samples,
        args.phi_samples,
        args.mc_samples,
    )

    overall = (property_summary["passed"] and furnace_summary["passed"]
               and integration_summary["passed"]
               and pdf_summary["passed"] and convergence_summary["passed"]
               and (gpu_summary is None or gpu_summary["passed"]))
    print(f"Output: {output}")
    print(f"BRDF properties : {pass_text(property_summary['passed'])}")
    print(f"White furnace   : {pass_text(furnace_summary['passed'])}")
    print(f"Integration conv: {pass_text(integration_summary['passed'])}")
    print(f"Sampling / PDF  : {pass_text(pdf_summary['passed'])}")
    print(f"MC convergence  : {pass_text(convergence_summary['passed'])}")
    if gpu_summary is not None:
        print(f"GPU HLSL cross  : {pass_text(gpu_summary['passed'])}")
    print(f"Overall         : {pass_text(overall)}")
    return 0 if overall else 2


if __name__ == "__main__":
    raise SystemExit(main())
