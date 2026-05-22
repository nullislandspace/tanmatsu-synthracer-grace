// hull
module hull() {
    rotate([-90, 0, 0]) {
        cylinder(h=5, d=3, $fn=6);
        translate([0, 0, 5]) {
            cylinder(h=5, d1=3, d2=0, $fn=6);
        }
    }
}

module wing() {
    difference() {
        translate([0, 0, -0.5]) {
            cube([5, 5, 1]);
        }
        translate([-1, 5, -1]) {
            rotate([0, 0, -30]) {
                cube([10, 10, 2]);
            }
        }
    }
    translate([5, -0.25, 0]) {
        rotate([-90, 0, 0]) {
            cylinder(h=6, d1=1.5, d2=0.5, $fn=6);
        }
    }
}

module body() {
    union() {
        hull();
        wing();
        
        mirror(v=[1, 0, 0]) {
            wing();
        }
    }
}

// battery panel
module batterypanel() {
    color("#0c0c0c") {
      translate([-0.5, 0.1, 1.35]) {
        cube([1, 4, 0.2]);
        }
    }
}

module chargeindicator(pos, col) {
    color(col) {
        translate([0, 0.55 + (pos*0.90), 1.55]) {
            cylinder(d=0.8, h=0.1, $fn=6);
        }
    }
}

body();
batterypanel();
chargeindicator(0, "#fffffa");
chargeindicator(1, "#fffffb");
chargeindicator(2, "#fffffc");
chargeindicator(3, "#fffffd");
