module pillars() {
    color("#a0a0a0") {
        translate([-5.5, 0, 0]) {
            cylinder(d=1, h=10, $fn=6);
        }
        translate([5.5, 0, 0]) {
            cylinder(d=1, h=10, $fn=8);
        }
    }
}

module signbase() {
    color("#ffffff") {
        translate([-4.5, -0.125, 7]) {
            cube([9, 0.25,  3]);
        }
    }
}

module signholders() {
    color("#0c0cFF") {
        translate([-5, -0.125, 9.7]) {
            cube([0.5, 0.25, 0.3]);
        }
        translate([-5, -0.125, 7]) {
            cube([0.5, 0.25, 0.3]);
        }
        translate([4.5, -0.125, 9.7]) {
            cube([0.5, 0.25, 0.3]);
        }
        translate([4.5, -0.125, 7]) {
            cube([0.5, 0.25, 0.3]);
        } 
    }
}
 
pillars();
signbase();
signholders();